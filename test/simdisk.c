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

void sim_init(SimDisk *d, const uint8_t *image, uint64_t len) {
  memset(d, 0, sizeof *d);
  d->crash_at = -1;
  d->fsync_fail_at = -1;
  ensure(&d->dur, &d->cap, len + 1);
  uint64_t c2 = 0;
  ensure(&d->vis, &c2, len + 1);
  if (len) { memcpy(d->dur, image, len); memcpy(d->vis, image, len); }
  d->dur_len = d->vis_len = len;
}
void sim_free(SimDisk *d) {
  for (int i = 0; i < d->npend; i++) free(d->pend[i].data);
  free(d->pend); free(d->dur); free(d->vis);
}

/* The visible image has its own capacity; keep it in step with the durable one. */
static void vis_ensure(SimDisk *d, uint64_t need) {
  static __thread int dummy;
  (void)dummy;
  uint64_t cap = d->cap; /* both buffers are always allocated with the same capacity */
  ensure(&d->vis, &cap, need);
  uint64_t cap2 = d->cap;
  ensure(&d->dur, &cap2, need);
  d->cap = cap > cap2 ? cap : cap2;
}

static int tick(SimDisk *d) {
  if (d->dead) return -1;
  if (d->crash_at >= 0 && d->events == d->crash_at) { d->dead = 1; return -1; }
  d->events++;
  return 0;
}

static int64_t s_size(void *c) { SimDisk *d = c; return d->dead ? -1 : (int64_t)d->vis_len; }
static int s_pread(void *c, uint64_t off, void *buf, size_t n) {
  SimDisk *d = c;
  if (d->dead) return -1;
  if (off >= d->vis_len) return 0;
  uint64_t m = d->vis_len - off;
  if (m > n) m = n;
  memcpy(buf, d->vis + off, m);
  return (int)m;
}
static void add_pending(SimDisk *d, int kind, uint64_t off, uint64_t len, const void *data) {
  if (d->npend == d->cappend) { d->cappend = d->cappend ? d->cappend * 2 : 16; d->pend = realloc(d->pend, (size_t)d->cappend * sizeof *d->pend); }
  SimOp *o = &d->pend[d->npend++];
  o->kind = kind; o->off = off; o->len = len; o->data = NULL;
  if (data) { o->data = malloc(len); memcpy(o->data, data, len); }
}
static int s_pwrite(void *c, uint64_t off, const void *buf, size_t n) {
  SimDisk *d = c;
  if (tick(d)) return -1;
  vis_ensure(d, off + n + 1);
  memcpy(d->vis + off, buf, n);
  if (off + n > d->vis_len) d->vis_len = off + n;
  add_pending(d, 0, off, n, buf);
  return 0;
}
static int s_truncate(void *c, uint64_t size) {
  SimDisk *d = c;
  if (tick(d)) return -1;
  vis_ensure(d, size + 1);
  if (size < d->vis_len) memset(d->vis + size, 0, d->vis_len - size);
  d->vis_len = size;
  add_pending(d, 1, size, 0, NULL);
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
static int s_fsync(void *c) {
  SimDisk *d = c;
  if (tick(d)) return -1;
  if (d->fsync_fail_at >= 0 && d->events - 1 == d->fsync_fail_at) return -1; /* pending stays pending */
  for (int i = 0; i < d->npend; i++) { apply_clean(&d->dur, &d->cap, &d->dur_len, &d->pend[i]); free(d->pend[i].data); }
  d->npend = 0;
  return 0;
}
fk_io sim_io(SimDisk *d) { fk_io io = { d, s_size, s_pread, s_pwrite, s_truncate, s_fsync }; return io; }

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

static void one_image(const SimDisk *d, Rng *r, uint32_t mask, int variant, sim_image_cb cb, void *ctx) {
  uint8_t *img = NULL;
  uint64_t cap = 0, len = d->dur_len;
  ensure(&img, &cap, d->dur_len + 1);
  memcpy(img, d->dur, d->dur_len);
  int n = d->npend, idx[64], m = 0;
  for (int i = 0; i < n && i < 64; i++) if ((i < 32 && (mask >> i) & 1) || (i >= 32 && rng_below(r, 2))) idx[m++] = i;
  for (int i = m - 1; i > 0; i--) { int j = (int)rng_below(r, (uint32_t)i + 1); int t = idx[i]; idx[i] = idx[j]; idx[j] = t; } /* any order */
  for (int k = 0; k < m; k++) apply_torn(&img, &cap, &len, &d->pend[idx[k]], r, variant == 1, variant == 1 && rng_below(r, 2));
  cb(ctx, img, len);
  free(img);
}

void sim_images(const SimDisk *d, Rng *r, sim_image_cb cb, void *ctx) {
  int n = d->npend;
  if (n <= 8) {
    for (uint32_t mask = 0; mask < (1u << n); mask++) for (int v = 0; v < 2; v++) one_image(d, r, mask, v, cb, ctx);
  } else {
    for (int t = 0; t < 128; t++) one_image(d, r, (uint32_t)rng_next(r), t & 1, cb, ctx);
    one_image(d, r, 0, 0, cb, ctx);
    one_image(d, r, 0xffffffffu, 0, cb, ctx);
  }
}
