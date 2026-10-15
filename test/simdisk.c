#include "simdisk.h"

#include <stdlib.h>
#include <string.h>

void rng_seed(Rng *r, uint64_t seed) { r->s = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull; if (!r->s) r->s = 1; }
uint64_t rng_next(Rng *r) {
  uint64_t x = r->s;
  x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
  r->s = x;
  return x * 0x2545F4914F6CDD1Dull;
}
uint32_t rng_below(Rng *r, uint32_t n) { return n ? (uint32_t)(rng_next(r) >> 33) % n : 0; }

static void ensure(uint8_t **buf, uint64_t *cap, uint64_t need) {
  if (need <= *cap) return;
  uint64_t nc = *cap ? *cap : 8192;
  while (nc < need) nc *= 2;
  *buf = realloc(*buf, nc);
  memset(*buf + *cap, 0, nc - *cap);
  *cap = nc;
}

static void file_reset(SimFile *f) {
  for (int i = 0; i < f->npend; i++) free(f->pend[i].data);
  free(f->pend); free(f->dur); free(f->vis);
  memset(f, 0, sizeof *f);
}
void sim_init(SimDisk *d, const uint8_t *image, uint64_t len) {
  memset(d, 0, sizeof *d);
  d->crash_at = -1;
  d->fsync_fail_at = -1;
  d->hm.d = d; d->hm.tmp = 0;
  d->ht.d = d; d->ht.tmp = 1;
  SimFile *f = &d->f[0];
  ensure(&f->dur, &f->dcap, len + 1);
  ensure(&f->vis, &f->vcap, len + 1);
  if (len) { memcpy(f->dur, image, len); memcpy(f->vis, image, len); }
  f->dur_len = f->vis_len = len;
}
void sim_free(SimDisk *d) { file_reset(&d->f[0]); file_reset(&d->f[1]); }
const uint8_t *sim_durable(const SimDisk *d, uint64_t *len) { *len = d->f[d->dur_main].dur_len; return d->f[d->dur_main].dur; }
const uint8_t *sim_visible(const SimDisk *d, uint64_t *len) { *len = d->f[d->vis_main].vis_len; return d->f[d->vis_main].vis; }

static SimFile *F(SimHandle *h) { SimDisk *d = h->d; return &d->f[h->tmp ? 1 - d->vis_main : d->vis_main]; }

static int tick(SimDisk *d) {
  if (d->dead) return -1;
  if (d->crash_at >= 0 && d->events == d->crash_at) { d->dead = 1; return -1; }
  d->events++;
  return 0;
}

static int64_t s_size(void *c) { SimHandle *h = c; return h->d->dead ? -1 : (int64_t)F(h)->vis_len; }
static int s_pread(void *c, uint64_t off, void *buf, size_t n) {
  SimHandle *h = c;
  if (h->d->dead) return -1;
  SimFile *f = F(h);
  if (off >= f->vis_len) return 0;
  uint64_t m = f->vis_len - off;
  if (m > n) m = n;
  memcpy(buf, f->vis + off, m);
  return (int)m;
}
static void add_pending(SimFile *f, int kind, uint64_t off, uint64_t len, const void *data) {
  if (f->npend == f->cappend) { f->cappend = f->cappend ? f->cappend * 2 : 16; f->pend = realloc(f->pend, (size_t)f->cappend * sizeof *f->pend); }
  SimOp *o = &f->pend[f->npend++];
  o->kind = kind; o->off = off; o->len = len; o->data = NULL;
  if (data) { o->data = malloc(len); memcpy(o->data, data, len); }
}
static int s_pwrite(void *c, uint64_t off, const void *buf, size_t n) {
  SimHandle *h = c;
  if (tick(h->d)) return -1;
  SimFile *f = F(h);
  ensure(&f->vis, &f->vcap, off + n + 1);
  memcpy(f->vis + off, buf, n);
  if (off + n > f->vis_len) f->vis_len = off + n;
  add_pending(f, 0, off, n, buf);
  return 0;
}
static int s_truncate(void *c, uint64_t size) {
  SimHandle *h = c;
  if (tick(h->d)) return -1;
  SimFile *f = F(h);
  ensure(&f->vis, &f->vcap, size + 1);
  if (size < f->vis_len) memset(f->vis + size, 0, f->vis_len - size);
  f->vis_len = size;
  add_pending(f, 1, size, 0, NULL);
  return 0;
}
static void apply_clean(uint8_t **img, uint64_t *cap, uint64_t *len, const SimOp *o) {
  if (o->kind == 0) {
    ensure(img, cap, o->off + o->len + 1);
    memcpy(*img + o->off, o->data, o->len);
    if (o->off + o->len > *len) *len = o->off + o->len;
  } else {
    ensure(img, cap, o->off + 1);
    if (o->off < *len) memset(*img + o->off, 0, *len - o->off);
    *len = o->off;
  }
}
static int fsync_fails(SimDisk *d) { return d->fsync_fail_at >= 0 && d->events - 1 == d->fsync_fail_at; }
static int s_fsync(void *c) {
  SimHandle *h = c;
  SimDisk *d = h->d;
  if (tick(d)) return -1;
  if (fsync_fails(d)) return -1; /* pending stays pending */
  if (d->nosync) return 0;      /* FK_FILE_NOSYNC: reported as done, nothing is made durable */
  SimFile *f = F(h);
  for (int i = 0; i < f->npend; i++) { apply_clean(&f->dur, &f->dcap, &f->dur_len, &f->pend[i]); free(f->pend[i].data); }
  f->npend = 0;
  return 0;
}

/* Sibling operations. Only one rename can be outstanding. */
static int s_sibling_create(void *c, fk_io *tmp) {
  SimHandle *h = c;
  SimDisk *d = h->d;
  if (tick(d)) return -1;
  if (d->vis_main != d->dur_main) return -1; /* the other file is still the durable one */
  file_reset(&d->f[1 - d->vis_main]);
  fk_io io = sim_io(d);
  io.ctx = &d->ht;
  io.sibling_create = NULL; io.sibling_rename = NULL; io.sync_dir = NULL; io.sibling_remove = NULL;
  *tmp = io;
  return 0;
}
static int s_sibling_rename(void *c, fk_io *tmp) {
  SimHandle *h = c;
  SimDisk *d = h->d;
  if (tick(d)) return -1;
  if (d->vis_main != d->dur_main) return -1;
  d->vis_main = 1 - d->vis_main; /* volatile namespace only: the main name now refers to the other file */
  tmp->ctx = NULL;
  return 0;
}
static int s_sync_dir(void *c) {
  SimHandle *h = c;
  SimDisk *d = h->d;
  if (tick(d)) return -1;
  if (fsync_fails(d)) return -1; /* the rename stays pending */
  if (d->nosync) return 0;
  d->dur_main = d->vis_main;
  return 0;
}
static void s_sibling_remove(void *c, fk_io *tmp) {
  SimHandle *h = c;
  SimDisk *d = h->d;
  (void)tmp;
  if (d->vis_main != d->dur_main) return; /* the old file is still the durable one: keep it */
  file_reset(&d->f[1 - d->vis_main]);
}
fk_io sim_io(SimDisk *d) {
  fk_io io = { &d->hm, s_size, s_pread, s_pwrite, s_truncate, s_fsync, s_sibling_create, s_sibling_rename, s_sync_dir, s_sibling_remove };
  return io;
}

/* Apply one pending op to a crash image, possibly torn. */
static void apply_torn(uint8_t **img, uint64_t *cap, uint64_t *len, const SimOp *o, Rng *r, int torn, int garbage) {
  if (o->kind == 1) { apply_clean(img, cap, len, o); return; }
  uint64_t lo = o->off, hi = o->off + o->len; /* bytes [lo,hi) survive, except what tearing removes */
  uint8_t *mask = NULL;
  if (torn) {
    mask = calloc(1, o->len);
    if (rng_below(r, 2) == 0) {
      uint64_t cut = rng_below(r, (uint32_t)o->len + 1); /* a prefix of the write */
      memset(mask, 1, cut);
    } else {
      for (uint64_t s = lo & ~511ull; s < hi; s += 512) { /* each 512-byte sector independently */
        if (rng_below(r, 2)) {
          uint64_t a = s < lo ? lo : s, b = s + 512 > hi ? hi : s + 512;
          memset(mask + (a - lo), 1, b - a);
        }
      }
    }
  }
  uint64_t top = lo; /* the file grows to cover the highest surviving byte */
  for (uint64_t i = 0; i < o->len; i++) if (!mask || mask[i]) top = lo + i + 1;
  if (top > *len) {
    ensure(img, cap, top + 1);
    if (garbage) for (uint64_t i = *len; i < top; i++) (*img)[i] = (uint8_t)rng_next(r);
    else memset(*img + *len, 0, top - *len);
    *len = top;
  }
  for (uint64_t i = 0; i < o->len; i++) if (!mask || mask[i]) (*img)[lo + i] = o->data[i];
  free(mask);
}

static void one_image(const SimFile *f, Rng *r, uint32_t mask, int variant, sim_image_cb cb, void *ctx) {
  uint8_t *img = NULL;
  uint64_t cap = 0, len = f->dur_len;
  ensure(&img, &cap, f->dur_len + 1);
  if (f->dur_len) memcpy(img, f->dur, f->dur_len);
  int n = f->npend, idx[1024], m = 0;
  for (int i = 0; i < n && i < 1024; i++) if ((i < 32 && (mask >> i) & 1) || (i >= 32 && rng_below(r, 2))) idx[m++] = i;
  for (int i = m - 1; i > 0; i--) { int j = (int)rng_below(r, (uint32_t)i + 1); int t = idx[i]; idx[i] = idx[j]; idx[j] = t; } /* any order */
  for (int k = 0; k < m; k++) apply_torn(&img, &cap, &len, &f->pend[idx[k]], r, variant == 1, variant == 1 && rng_below(r, 2));
  cb(ctx, img, len);
  free(img);
}

static void file_images(const SimFile *f, Rng *r, sim_image_cb cb, void *ctx) {
  int n = f->npend;
  if (n <= 8) {
    for (uint32_t mask = 0; mask < (1u << n); mask++) for (int v = 0; v < 2; v++) one_image(f, r, mask, v, cb, ctx);
  } else {
    for (int t = 0; t < 128; t++) one_image(f, r, (uint32_t)rng_next(r), t & 1, cb, ctx);
    one_image(f, r, 0, 0, cb, ctx);
    one_image(f, r, 0xffffffffu, 0, cb, ctx);
  }
}

void sim_images(const SimDisk *d, Rng *r, sim_image_cb cb, void *ctx) {
  file_images(&d->f[d->dur_main], r, cb, ctx); /* the directory change, if any, was lost */
  if (d->vis_main != d->dur_main) file_images(&d->f[d->vis_main], r, cb, ctx); /* ... or it survived */
}

/* The first m pending writes in order, the last torn or whole: see sim_images_prefix. */
static void prefix_image(const SimFile *f, Rng *r, int m, int torn, sim_image_cb cb, void *ctx) {
  uint8_t *img = NULL;
  uint64_t cap = 0, len = f->dur_len;
  ensure(&img, &cap, f->dur_len + 1);
  if (f->dur_len) memcpy(img, f->dur, f->dur_len);
  for (int k = 0; k < m; k++) apply_torn(&img, &cap, &len, &f->pend[k], r, torn && k == m - 1, 0);
  cb(ctx, img, len);
  free(img);
}
static void file_prefix_images(const SimFile *f, Rng *r, int samples, sim_image_cb cb, void *ctx) {
  int n = f->npend;
  int step = samples > 0 && n > samples ? n / samples : 1;
  for (int m = 0; m <= n; m += step) {
    prefix_image(f, r, m, 0, cb, ctx);
    if (m > 0) prefix_image(f, r, m, 1, cb, ctx);
  }
  if (n % step != 0) { prefix_image(f, r, n, 0, cb, ctx); prefix_image(f, r, n, 1, cb, ctx); }
}
void sim_images_prefix(const SimDisk *d, Rng *r, int samples, sim_image_cb cb, void *ctx) {
  file_prefix_images(&d->f[d->dur_main], r, samples, cb, ctx);
  if (d->vis_main != d->dur_main) file_prefix_images(&d->f[d->vis_main], r, samples, cb, ctx);
}
