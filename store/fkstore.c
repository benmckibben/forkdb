/* fkstore.c: see docs/DURABLE-STORE.md for the format and the recovery argument. */
#include "fkstore.h"

#include <stdlib.h>
#include <string.h>

#define MAGIC 0x31524b46u /* "FKR1" little-endian */
#define HDR_SZ 4096u
#define REC_HDR 32u
#define PAGE_PAYLOAD (8u + FK_PAGE)
#define COMMIT_PAYLOAD 20u
#define TOMB UINT64_MAX
#define FK_MARK_EVERY 32u /* durable-end marker: refresh after this many committed units (and on close) */

enum { T_PAGE = 1, T_COMMIT = 2, T_FORK = 3, T_DISCARD = 4 };

/* ---------- checksums and encoding ---------- */
static uint32_t crc_tab[256];
static int crc_ready;
static void crc_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    crc_tab[i] = c;
  }
  crc_ready = 1;
}
uint32_t fk_crc32c(uint32_t crc, const void *p, size_t n) {
  if (!crc_ready) crc_init();
  const uint8_t *b = p;
  uint32_t c = ~crc;
  while (n--) c = crc_tab[(c ^ *b++) & 0xff] ^ (c >> 8);
  return ~c;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
__attribute__((unused)) static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

/* ---------- in-memory layers (rebuilt from the log on open) ---------- */
typedef struct Layer {
  struct Layer *parent;
  uint64_t *off; /* page number -> offset of its PAGE record; 0 = not in this layer; TOMB = zero page */
  uint32_t cap, size, touched;
} Layer;

typedef struct { Layer *head; int alive; } Branch;
typedef struct { uint32_t pgno; uint64_t off; uint8_t *data; } PendPage;

struct fk_store {
  fk_io io;
  int failed;
  uint64_t end, seq; /* append position and next sequence number of the committed log */
  uint32_t last_crc; /* crc of the last committed record (the prev_crc of the next record) */
  uint64_t mark_end, mark_gen; /* the durable-end marker: log bytes known to have been durably committed */
  uint32_t since_mark;
  Branch br[FK_MAX_BRANCHES];
  uint32_t nb;
  Layer **layers;
  size_t nlayers, caplayers;
  int txn_open;
  uint32_t txn_branch;
  PendPage *pp;
  uint32_t npp, cpp;
};

static Layer *layer_new(fk_store *s, Layer *parent) {
  Layer *l = calloc(1, sizeof *l);
  if (!l) return NULL;
  if (s->nlayers == s->caplayers) {
    size_t nc = s->caplayers ? s->caplayers * 2 : 16;
    Layer **n = realloc(s->layers, nc * sizeof *n);
    if (!n) { free(l); return NULL; }
    s->layers = n;
    s->caplayers = nc;
  }
  s->layers[s->nlayers++] = l;
  l->parent = parent;
  if (parent) l->size = parent->size;
  return l;
}
static int layer_set(Layer *l, uint32_t pgno, uint64_t v) {
  if (pgno >= l->cap) {
    uint32_t nc = l->cap ? l->cap : 16;
    while (nc <= pgno) nc *= 2;
    uint64_t *n = realloc(l->off, (size_t)nc * sizeof *n);
    if (!n) return FK_ENOMEM;
    memset(n + l->cap, 0, (size_t)(nc - l->cap) * sizeof *n);
    l->off = n;
    l->cap = nc;
  }
  if (!l->off[pgno]) l->touched++;
  l->off[pgno] = v;
  return FK_OK;
}
static uint64_t layer_lookup(const Layer *l, uint32_t pgno) {
  for (; l; l = l->parent)
    if (pgno < l->cap && l->off[pgno]) return l->off[pgno];
  return 0;
}

static int apply_commit(fk_store *s, uint32_t b, const PendPage *pp, uint32_t n, uint32_t new_size) {
  Layer *h = s->br[b].head;
  int rc;
  for (uint32_t pg = new_size; pg < h->size; pg++)
    if ((rc = layer_set(h, pg, TOMB)) != FK_OK) return rc;
  for (uint32_t i = 0; i < n; i++)
    if ((rc = layer_set(h, pp[i].pgno, pp[i].off)) != FK_OK) return rc;
  h->size = new_size;
  return FK_OK;
}
static int apply_fork(fk_store *s, uint32_t src, uint32_t dst) {
  Layer *f = s->br[src].head;
  if (f->touched == 0 && f->parent && f->size == f->parent->size) {
    f = f->parent; /* nothing changed since the last fork: share the parent instead of stacking an empty layer */
  } else {
    Layer *nh = layer_new(s, f);
    if (!nh) return FK_ENOMEM;
    s->br[src].head = nh;
  }
  Layer *d = layer_new(s, f);
  if (!d) return FK_ENOMEM;
  s->br[dst].head = d;
  s->br[dst].alive = 1;
  s->nb = dst + 1;
  return FK_OK;
}

/* ---------- log records ---------- */
static int put_rec(fk_store *s, uint16_t type, const uint8_t *pay, uint32_t len, uint64_t *w_end, uint64_t *w_seq, uint32_t *w_last,
                   uint64_t *rec_off) {
  uint8_t *buf = calloc(1, REC_HDR + (size_t)len);
  if (!buf) return FK_ENOMEM;
  put32(buf, MAGIC);
  put16(buf + 4, type);
  put64(buf + 8, *w_seq);
  put32(buf + 16, len);
  put32(buf + 20, *w_last);
  memcpy(buf + REC_HDR, pay, len);
  uint32_t c = fk_crc32c(0, buf, REC_HDR); /* the crc field is still zero here */
  c = fk_crc32c(c, buf + REC_HDR, len);
  put32(buf + 24, c);
  if (s->io.pwrite(s->io.ctx, *w_end, buf, REC_HDR + (size_t)len) != 0) {
    free(buf);
    s->failed = 1;
    return FK_EIO;
  }
  if (rec_off) *rec_off = *w_end;
  *w_end += REC_HDR + (uint64_t)len;
  (*w_seq)++;
  *w_last = c;
  free(buf);
  return FK_OK;
}
static int do_sync(fk_store *s) {
#ifdef FK_BUG_SKIP_FSYNC
  (void)s;
  return FK_OK; /* mutant: acknowledge before the data is durable */
#else
  if (s->io.fsync(s->io.ctx) != 0) {
    s->failed = 1;
    return FK_EIO;
  }
  return FK_OK;
#endif
}

/* ---------- durable-end marker ----------
 * A log alone cannot tell a torn tail (normal after a power cut: cut it off) from damage inside data that was once
 * durable (bit rot: must not be silently cut off). The marker records how far the log was durably committed. It is written
 * only AFTER the data it covers has been fsynced, into one of two alternating 32-byte slots (each in its own sector), so a
 * power cut can only leave an older marker, never one that points past durable data. */
static void read_marker(fk_store *s) {
  uint64_t best_gen = 0, best_end = HDR_SZ;
  for (unsigned slot = 0; slot < 2; slot++) {
    uint8_t m[32];
    if (s->io.pread(s->io.ctx, 512u * (1 + slot), m, 32) != 32) continue;
    if (memcmp(m, "FKMK", 4) != 0 || get32(m + 4) != fk_crc32c(0, m + 8, 16)) continue;
    uint64_t g = get64(m + 8), e = get64(m + 16);
    if (g > best_gen && e >= HDR_SZ) { best_gen = g; best_end = e; }
  }
  s->mark_gen = best_gen;
  s->mark_end = best_end;
}
static int write_marker(fk_store *s, uint64_t end, int sync) {
  uint8_t m[32];
  memset(m, 0, sizeof m);
  memcpy(m, "FKMK", 4);
  put64(m + 8, s->mark_gen + 1);
  put64(m + 16, end);
  put32(m + 4, fk_crc32c(0, m + 8, 16));
  if (s->io.pwrite(s->io.ctx, 512u * (1 + (unsigned)((s->mark_gen + 1) & 1)), m, 32) != 0) return FK_EIO;
  if (sync && s->io.fsync(s->io.ctx) != 0) return FK_EIO;
  s->mark_gen++;
  s->mark_end = end;
  return FK_OK;
}
static void maybe_mark(fk_store *s) {
  if (++s->since_mark < FK_MARK_EVERY) return;
  if (write_marker(s, s->end, 1) == FK_OK) s->since_mark = 0;
  else s->failed = 1; /* the commit itself is durable and acknowledged; further use needs a reopen */
}

/* ---------- open and recovery ---------- */
static int write_header(fk_store *s) {
  uint8_t h[HDR_SZ];
  memset(h, 0, sizeof h);
  memcpy(h, "FORKDBST", 8);
  put32(h + 8, 1);
  put32(h + 12, FK_PAGE);
  put32(h + 16, fk_crc32c(0, h, 16));
  if (s->io.truncate(s->io.ctx, 0) != 0) return FK_EIO;
  if (s->io.pwrite(s->io.ctx, 0, h, sizeof h) != 0) return FK_EIO;
  if (s->io.fsync(s->io.ctx) != 0) return FK_EIO;
  return FK_OK;
}
static int header_ok(const uint8_t *h) { return memcmp(h, "FORKDBST", 8) == 0 && get32(h + 8) == 1 && get32(h + 12) == FK_PAGE && get32(h + 16) == fk_crc32c(0, h, 16); }

static int scan(fk_store *s, int64_t fsize) {
  uint64_t off = HDR_SZ, c_end = HDR_SZ, seq = 1, c_seq = 1;
  uint32_t last = 0, c_last = 0, npend = 0, cpend = 0, pbranch = 0;
  PendPage *pend = NULL;
  uint8_t h[REC_HDR];
  uint8_t pay[PAGE_PAYLOAD];
  int rc = FK_OK;
  for (;;) {
    if (s->io.pread(s->io.ctx, off, h, REC_HDR) != (int)REC_HDR) break;
    uint32_t len = get32(h + 16), crc = get32(h + 24);
    uint16_t type = get16(h + 4);
    if (get32(h) != MAGIC || len > PAGE_PAYLOAD) break;
#ifndef FK_BUG_NO_PREVCHAIN
    if (get64(h + 8) != seq || get32(h + 20) != last) break;
#endif
    if (len && s->io.pread(s->io.ctx, off + REC_HDR, pay, len) != (int)len) break;
    uint8_t hc[REC_HDR];
    memcpy(hc, h, REC_HDR);
    put32(hc + 24, 0);
    uint32_t c = fk_crc32c(fk_crc32c(0, hc, REC_HDR), pay, len);
    if (c != crc) break;

    int unit = 0; /* 1 when this record completes a committed unit */
    if (type == T_PAGE) {
      if (len != PAGE_PAYLOAD) break;
      uint32_t b = get32(pay);
      if (b >= s->nb || !s->br[b].alive || (npend && b != pbranch)) break;
      if (npend == cpend) {
        uint32_t nc = cpend ? cpend * 2 : 8;
        PendPage *n = realloc(pend, nc * sizeof *n);
        if (!n) { rc = FK_ENOMEM; break; }
        pend = n;
        cpend = nc;
      }
      pend[npend].pgno = get32(pay + 4);
      pend[npend].off = off;
      pend[npend].data = NULL;
      npend++;
      pbranch = b;
    } else if (type == T_COMMIT) {
      if (len != COMMIT_PAYLOAD) break;
      uint32_t b = get32(pay), np = get32(pay + 4), nsize = get32(pay + 8);
      if (b >= s->nb || !s->br[b].alive || np != npend || (npend && b != pbranch)) break;
      int bad = 0;
      for (uint32_t i = 0; i < npend; i++) if (pend[i].pgno >= nsize) bad = 1;
      if (bad) break;
      if ((rc = apply_commit(s, b, pend, npend, nsize)) != FK_OK) break;
      npend = 0;
      unit = 1;
    } else if (type == T_FORK) {
      if (len != 8 || npend) break;
      uint32_t src = get32(pay), dst = get32(pay + 4);
      if (src >= s->nb || !s->br[src].alive || dst != s->nb || dst >= FK_MAX_BRANCHES) break;
      if ((rc = apply_fork(s, src, dst)) != FK_OK) break;
      unit = 1;
    } else if (type == T_DISCARD) {
      if (len != 4 || npend) break;
      uint32_t b = get32(pay);
      if (b == 0 || b >= s->nb || !s->br[b].alive) break;
      s->br[b].alive = 0;
      unit = 1;
    } else {
      break;
    }
    off += REC_HDR + (uint64_t)len;
    seq++;
    last = c;
    if (unit) { c_end = off; c_seq = seq; c_last = last; }
  }
  free(pend);
  if (rc != FK_OK) return rc;
  if (c_end < s->mark_end) return FK_ECORRUPT; /* data that was durably committed is gone or damaged: do not cut anything off */
  s->end = c_end;
  s->seq = c_seq;
  s->last_crc = c_last;
  if ((uint64_t)fsize > c_end) {
    /* A torn or uncommitted tail. Remove it so that new records start on a clean boundary. */
#ifndef FK_BUG_NO_TRUNC
    if (s->io.truncate(s->io.ctx, c_end) != 0) return FK_EIO;
    if (s->io.fsync(s->io.ctx) != 0) return FK_EIO;
#endif
  }
  return FK_OK;
}

int fk_open(const fk_io *io, fk_store **out) {
  fk_store *s = calloc(1, sizeof *s);
  if (!s) return FK_ENOMEM;
  s->io = *io;
  int rc;
  int64_t size = io->size(io->ctx);
  if (size < 0) { free(s); return FK_EIO; }
  uint8_t h[HDR_SZ];
  int fresh = 0;
  if (size < (int64_t)HDR_SZ) {
    fresh = 1; /* new file, or a creation that was cut short (records are only written after the header is durable) */
  } else {
    if (io->pread(io->ctx, 0, h, HDR_SZ) != (int)HDR_SZ) { free(s); return FK_EIO; }
    if (!header_ok(h)) {
      if (size == (int64_t)HDR_SZ) fresh = 1; /* header sector never landed: still a creation in progress */
      else { free(s); return FK_ECORRUPT; }
    }
  }
  if (fresh) {
    if ((rc = write_header(s)) != FK_OK) { free(s); return rc; }
    size = HDR_SZ;
  }
  if (fresh) { s->mark_end = HDR_SZ; s->mark_gen = 0; } else read_marker(s);
  Layer *main_layer = layer_new(s, NULL);
  if (!main_layer) { fk_close(s); return FK_ENOMEM; }
  s->br[0].head = main_layer;
  s->br[0].alive = 1;
  s->nb = 1;
  if ((rc = scan(s, size)) != FK_OK) { fk_close(s); return rc; }
  *out = s;
  return FK_OK;
}

void fk_close(fk_store *s) {
  if (!s) return;
  if (!s->failed && s->end > s->mark_end) (void)write_marker(s, s->end, 1);
  for (size_t i = 0; i < s->nlayers; i++) { free(s->layers[i]->off); free(s->layers[i]); }
  free(s->layers);
  for (uint32_t i = 0; i < s->npp; i++) free(s->pp[i].data);
  free(s->pp);
  free(s);
}

/* ---------- queries ---------- */
uint32_t fk_branch_count(const fk_store *s) { return s->nb; }
int fk_branch_alive(const fk_store *s, uint32_t b) { return b < s->nb && s->br[b].alive; }
int fk_size(const fk_store *s, uint32_t b, uint32_t *pages) {
  if (!fk_branch_alive(s, b)) return FK_ENOENT;
  *pages = s->br[b].head->size;
  return FK_OK;
}
int fk_read(fk_store *s, uint32_t b, uint32_t pgno, void *buf) {
  if (s->failed) return FK_EFAILED;
  if (!fk_branch_alive(s, b)) return FK_ENOENT;
  if (pgno >= s->br[b].head->size) return FK_ENOENT;
  uint64_t off = layer_lookup(s->br[b].head, pgno);
  if (off == 0 || off == TOMB) { memset(buf, 0, FK_PAGE); return FK_OK; }
  uint8_t rec[REC_HDR + PAGE_PAYLOAD];
  if (s->io.pread(s->io.ctx, off, rec, sizeof rec) != (int)sizeof rec) return FK_ECORRUPT;
  uint8_t hc[REC_HDR];
  memcpy(hc, rec, REC_HDR);
  put32(hc + 24, 0);
  uint32_t c = fk_crc32c(fk_crc32c(0, hc, REC_HDR), rec + REC_HDR, PAGE_PAYLOAD);
  if (get32(rec) != MAGIC || get16(rec + 4) != T_PAGE || c != get32(rec + 24) || get32(rec + REC_HDR + 4) != pgno) return FK_ECORRUPT;
  memcpy(buf, rec + REC_HDR + 8, FK_PAGE);
  return FK_OK;
}

/* ---------- writes ---------- */
static void commit_tail(fk_store *s, uint64_t end, uint64_t seq, uint32_t last) { s->end = end; s->seq = seq; s->last_crc = last; }

int fk_fork(fk_store *s, uint32_t src, uint32_t *dst) {
  if (s->failed) return FK_EFAILED;
  if (s->txn_open) return FK_EBUSY;
  if (!fk_branch_alive(s, src) || s->nb >= FK_MAX_BRANCHES) return FK_EINVAL;
  uint32_t id = s->nb;
  uint8_t pay[8];
  put32(pay, src);
  put32(pay + 4, id);
  uint64_t e = s->end, q = s->seq;
  uint32_t l = s->last_crc;
  int rc = put_rec(s, T_FORK, pay, 8, &e, &q, &l, NULL);
  if (rc == FK_OK) rc = do_sync(s);
  if (rc != FK_OK) return rc;
  commit_tail(s, e, q, l);
  if ((rc = apply_fork(s, src, id)) != FK_OK) { s->failed = 1; return rc; }
  maybe_mark(s);
  *dst = id;
  return FK_OK;
}

int fk_discard(fk_store *s, uint32_t b) {
  if (s->failed) return FK_EFAILED;
  if (s->txn_open) return FK_EBUSY;
  if (b == 0 || !fk_branch_alive(s, b)) return FK_EINVAL;
  uint8_t pay[4];
  put32(pay, b);
  uint64_t e = s->end, q = s->seq;
  uint32_t l = s->last_crc;
  int rc = put_rec(s, T_DISCARD, pay, 4, &e, &q, &l, NULL);
  if (rc == FK_OK) rc = do_sync(s);
  if (rc != FK_OK) return rc;
  commit_tail(s, e, q, l);
  s->br[b].alive = 0;
  maybe_mark(s);
  return FK_OK;
}

int fk_txn_begin(fk_store *s, uint32_t b) {
  if (s->failed) return FK_EFAILED;
  if (s->txn_open) return FK_EBUSY;
  if (!fk_branch_alive(s, b)) return FK_ENOENT;
  s->txn_open = 1;
  s->txn_branch = b;
  return FK_OK;
}
int fk_txn_write(fk_store *s, uint32_t pgno, const void *page) {
  if (!s->txn_open) return FK_EINVAL;
  if (s->npp == s->cpp) {
    uint32_t nc = s->cpp ? s->cpp * 2 : 8;
    PendPage *n = realloc(s->pp, nc * sizeof *n);
    if (!n) return FK_ENOMEM;
    s->pp = n;
    s->cpp = nc;
  }
  uint8_t *d = malloc(FK_PAGE);
  if (!d) return FK_ENOMEM;
  memcpy(d, page, FK_PAGE);
  s->pp[s->npp].pgno = pgno;
  s->pp[s->npp].off = 0;
  s->pp[s->npp].data = d;
  s->npp++;
  return FK_OK;
}
void fk_txn_abort(fk_store *s) {
  for (uint32_t i = 0; i < s->npp; i++) free(s->pp[i].data);
  s->npp = 0;
  s->txn_open = 0;
}
int fk_txn_commit(fk_store *s, uint32_t new_size) {
  if (s->failed) return FK_EFAILED;
  if (!s->txn_open) return FK_EINVAL;
  for (uint32_t i = 0; i < s->npp; i++)
    if (s->pp[i].pgno >= new_size) return FK_EINVAL; /* the transaction stays open; the caller may abort */
  uint64_t e = s->end, q = s->seq;
  uint32_t l = s->last_crc;
  int rc = FK_OK;
  uint8_t pay[PAGE_PAYLOAD];
  for (uint32_t i = 0; i < s->npp && rc == FK_OK; i++) {
    put32(pay, s->txn_branch);
    put32(pay + 4, s->pp[i].pgno);
    memcpy(pay + 8, s->pp[i].data, FK_PAGE);
    rc = put_rec(s, T_PAGE, pay, PAGE_PAYLOAD, &e, &q, &l, &s->pp[i].off);
  }
  if (rc == FK_OK) {
    uint8_t cp[COMMIT_PAYLOAD];
    memset(cp, 0, sizeof cp);
    put32(cp, s->txn_branch);
    put32(cp + 4, s->npp);
    put32(cp + 8, new_size);
    rc = put_rec(s, T_COMMIT, cp, COMMIT_PAYLOAD, &e, &q, &l, NULL);
  }
#ifdef FK_BUG_EARLY_MARKER
  if (rc == FK_OK) (void)write_marker(s, e, 0); /* mutant: marker issued before the data it covers is durable */
#endif
  if (rc == FK_OK) rc = do_sync(s);
  if (rc != FK_OK) return rc; /* fail-stop: the transaction may or may not be on disk; recovery decides */
  commit_tail(s, e, q, l);
  rc = apply_commit(s, s->txn_branch, s->pp, s->npp, new_size);
  if (rc != FK_OK) s->failed = 1;
  fk_txn_abort(s);
  if (rc == FK_OK) maybe_mark(s);
  return rc;
}
