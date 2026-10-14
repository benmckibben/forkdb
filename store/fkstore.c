/* fkstore.c: see docs/DURABLE-STORE.md for the format, the recovery argument and compaction. */
#include "fkstore.h"

#include <stdlib.h>
#include <string.h>
#ifdef OOM_INJECT
#include "oom_harness.h" /* after the system headers; shares oom_counter with the test */
#endif

#define MAGIC 0x31524b46u /* "FKR1" little-endian */
#define FK_VERSION 4u     /* 2 added snapshots (compaction); 3 lets fork reuse the ids of discarded branches and adds the allocation cursor to snapshots; 4 adds branch metadata, reset and state ids (FORKM, RESET, META records; the snapshot ends with the state counter and, per branch, its state id and metadata). Versions 1 to 3 are still read. */
#define HDR_SZ FK_HEADER_SZ
#define REC_HDR 32u
#define PAGE_PAYLOAD (8u + FK_PAGE)
#define REC_PAGE_SZ (REC_HDR + PAGE_PAYLOAD)
#define COMMIT_PAYLOAD 20u
#define TOMB UINT64_MAX
#define NONE UINT32_MAX
#define FK_MARK_EVERY 32u /* durable-end marker: refresh after this many committed units (and on close) */
#define FK_MAX_SNAP (1u << 30)
#define FK_CHECKPOINT_MIN_BYTES ((uint64_t)16 << 20)

enum { T_PAGE = 1, T_COMMIT = 2, T_FORK = 3, T_DISCARD = 4, T_CPAGE = 5, T_SNAP = 6, T_SNAPEND = 7, T_FORKM = 8, T_RESET = 9, T_META = 10 };

/* ---------- checksums and encoding ---------- */
/* CRC-32C (Castagnoli). Two implementations of the same function: a portable table loop, and the CPU's CRC32 instruction where the
 * build and the CPU have it (x86-64 with SSE4.2, selected at run time; AArch64 when compiled with the CRC extension). The results are
 * identical, so files written with one are read with the other. */
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
static uint32_t crc_sw(uint32_t crc, const void *p, size_t n) {
  if (!crc_ready) crc_init();
  const uint8_t *b = p;
  uint32_t c = ~crc;
  while (n--) c = crc_tab[(c ^ *b++) & 0xff] ^ (c >> 8);
  return ~c;
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#define FK_HAVE_HWCRC 1
__attribute__((target("sse4.2"))) static uint32_t crc_hw(uint32_t crc, const void *p, size_t n) {
  const uint8_t *b = p;
  uint64_t c = (uint32_t)~crc;
  while (n >= 8) {
    uint64_t v;
    memcpy(&v, b, 8); /* the buffer need not be aligned */
    c = _mm_crc32_u64(c, v);
    b += 8;
    n -= 8;
  }
  uint32_t c32 = (uint32_t)c;
#ifndef FK_BUG_CRC_HW_TAIL
  while (n--) c32 = _mm_crc32_u8(c32, *b++);
#endif /* mutant: the last n % 8 bytes are left out of the checksum */
  return ~c32;
}
static int crc_hw_usable(void) {
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse4.2") != 0;
}
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define FK_HAVE_HWCRC 1
static uint32_t crc_hw(uint32_t crc, const void *p, size_t n) {
  const uint8_t *b = p;
  uint32_t c = ~crc;
  while (n >= 8) {
    uint64_t v;
    memcpy(&v, b, 8);
    c = __crc32cd(c, v);
    b += 8;
    n -= 8;
  }
  while (n--) c = __crc32cb(c, *b++);
  return ~c;
}
static int crc_hw_usable(void) { return 1; }
#endif

typedef uint32_t (*crc_fn)(uint32_t, const void *, size_t);
static crc_fn crc_impl; /* chosen on first use */
int fk_crc32c_select(int want_hw) {
  crc_impl = crc_sw;
  int hw = 0;
#ifdef FK_HAVE_HWCRC
  if (want_hw && crc_hw_usable()) { crc_impl = crc_hw; hw = 1; }
#else
  (void)want_hw;
#endif
  return hw;
}
uint32_t fk_crc32c(uint32_t crc, const void *p, size_t n) {
  if (!crc_impl) fk_crc32c_select(1);
  return crc_impl(crc, p, n);
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

/* ---------- in-memory layers (rebuilt from the log on open) ---------- */
/* A layer maps page number -> offset of the PAGE record holding that page's content in this layer (or TOMB: the page reads as
 * zeros). It is an open-addressing hash table with linear probing, kept at most half full, so its memory is proportional to the
 * number of pages the layer holds, not to the highest page number it touched. */
#define KEY_EMPTY UINT32_MAX
typedef struct Layer {
  struct Layer *parent;
  uint32_t *key; /* page number, or KEY_EMPTY; NULL until the first entry */
  uint64_t *val;
  uint32_t cap, count, size; /* cap is 0 or a power of two; count entries; size = database size in pages */
  uint8_t bits;              /* log2(cap) */
  /* The layer tree, kept up to date as branches are forked and discarded (see layer_settle): */
  struct Layer *kid, *sib, *sibp; /* first child; next and previous sibling in the parent's list of children */
  uint32_t nkids;                  /* number of children */
  uint32_t nheads;                 /* number of live branches whose head this layer is */
  uint32_t slot;                   /* index in the store's array of layers */
  uint32_t kids, heads, idx;       /* scratch for compaction planning */
  uint8_t seen;
} Layer;

/* state: identifies the content of the branch (see fk_state); meta: the small blob the layer above keeps with the branch (see fk_meta_set) */
typedef struct { Layer *head; int alive; uint64_t state; uint8_t *meta; uint16_t nmeta; } Branch;
typedef struct { uint32_t pgno; uint64_t off; uint8_t *data; } PendPage;

struct fk_store {
  fk_io io;
  int failed;
  uint64_t end, seq; /* append position and next sequence number of the committed log */
  uint32_t last_crc; /* crc of the last committed record (the prev_crc of the next record) */
  uint64_t mark_end, mark_gen; /* the durable-end marker: log bytes known to have been durably committed */
  uint32_t since_mark;
  Branch br[FK_MAX_BRANCHES];
  uint32_t nb;        /* one more than the highest branch id ever in use */
  uint32_t last_id;   /* the id the latest fork was given: the allocation cursor (see alloc_id) */
  uint32_t version;   /* of the file: reuse of ids is only allowed from version 3, metadata, reset and state ids from version 4 */
  uint64_t state_ctr; /* the last state id handed out: every commit takes the next one (a state id is never given to two different contents) */
  Layer **layers;
  size_t nlayers, caplayers;
  int txn_open;
  uint32_t txn_branch;
  PendPage *pp;
  uint32_t npp, cpp;
};

static void layer_link(Layer *l, Layer *parent) { /* make l a child of parent */
  l->parent = parent;
  l->sib = l->sibp = NULL;
  if (!parent) return;
  l->sib = parent->kid;
  if (parent->kid) parent->kid->sibp = l;
  parent->kid = l;
  parent->nkids++;
}
static void layer_unlink(Layer *l) { /* detach l from its parent (l keeps its own children) */
  Layer *p = l->parent;
  if (!p) return;
  if (l->sibp) l->sibp->sib = l->sib; else p->kid = l->sib;
  if (l->sib) l->sib->sibp = l->sibp;
  p->nkids--;
  l->parent = NULL;
  l->sib = l->sibp = NULL;
}
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
  l->slot = (uint32_t)s->nlayers;
  s->layers[s->nlayers++] = l;
  layer_link(l, parent);
  if (parent) l->size = parent->size;
  return l;
}
static void layer_free(Layer *l) {
  if (!l) return;
  free(l->key);
  free(l->val);
  free(l);
}
static inline uint32_t lhash(uint32_t pgno, unsigned bits) { return (uint32_t)(pgno * 2654435769u) >> (32 - bits); } /* Fibonacci hashing: the top bits */

/* Make room for at least n entries at half load. On failure the layer is unchanged. */
static int layer_reserve(Layer *l, uint32_t n) {
  uint64_t want = (uint64_t)n * 2;
  if (want <= l->cap) return FK_OK;
  uint32_t nc = l->cap ? l->cap : 8;
  while (nc < want) nc *= 2;
  unsigned bits = 0;
  while ((1u << bits) < nc) bits++;
  uint32_t *nk = malloc((size_t)nc * sizeof *nk);
  uint64_t *nv = malloc((size_t)nc * sizeof *nv);
  if (!nk || !nv) { free(nk); free(nv); return FK_ENOMEM; }
  memset(nk, 0xff, (size_t)nc * sizeof *nk); /* KEY_EMPTY */
  uint32_t mask = nc - 1;
#ifdef FK_BUG_INDEX_GROW_DROP
  for (uint32_t i = 0; i + 1 < l->cap; i++) { /* mutant: the last old slot is not moved */
#else
  for (uint32_t i = 0; i < l->cap; i++) {
#endif
    if (l->key[i] == KEY_EMPTY) continue;
    uint32_t j = lhash(l->key[i], bits);
    while (nk[j] != KEY_EMPTY) j = (j + 1) & mask;
    nk[j] = l->key[i];
    nv[j] = l->val[i];
  }
  free(l->key);
  free(l->val);
  l->key = nk;
  l->val = nv;
  l->cap = nc;
  l->bits = (uint8_t)bits;
  return FK_OK;
}
static int layer_set(Layer *l, uint32_t pgno, uint64_t v) {
  if (pgno >= FK_MAX_PAGES) return FK_EINVAL;
  if (l->cap) {
    uint32_t mask = l->cap - 1, i = lhash(pgno, l->bits);
    for (; l->key[i] != KEY_EMPTY; i = (i + 1) & mask) {
#ifndef FK_BUG_INDEX_DUP_ON_OVERWRITE
      if (l->key[i] == pgno) { l->val[i] = v; return FK_OK; } /* mutant: an overwrite adds a second entry instead */
#endif
    }
  }
  int rc = layer_reserve(l, l->count + 1);
  if (rc != FK_OK) return rc;
  uint32_t mask = l->cap - 1, i = lhash(pgno, l->bits);
  while (l->key[i] != KEY_EMPTY) i = (i + 1) & mask;
  l->key[i] = pgno;
  l->val[i] = v;
  l->count++;
  return FK_OK;
}
/* 0 = this layer has no entry for the page */
static inline uint64_t layer_get(const Layer *l, uint32_t pgno) {
  if (!l->cap) return 0;
  uint32_t i = lhash(pgno, l->bits);
#ifdef FK_BUG_INDEX_NO_PROBE
  return l->key[i] == pgno ? l->val[i] : 0; /* mutant: a collision is treated as a miss */
#else
  for (uint32_t mask = l->cap - 1; l->key[i] != KEY_EMPTY; i = (i + 1) & mask)
    if (l->key[i] == pgno) return l->val[i];
  return 0;
#endif
}
static uint64_t layer_lookup(const Layer *l, uint32_t pgno) {
  for (; l; l = l->parent) {
    uint64_t v = layer_get(l, pgno);
    if (v) return v;
  }
  return 0;
}

/* ---------- keeping the layer tree small ----------
 * Invariant, restored after every discard: every layer has a live branch on it (nheads > 0) or a child (nkids > 0), and no layer
 * with nheads == 0 has exactly one child. A layer with neither head nor child is dead and is freed. A layer with one child and
 * no head is only a stored copy of the pages that child builds on, so it is folded into the child: the two become one layer, the
 * smaller one's entries are moved into the larger. This is what keeps a read from walking a chain of layers that no branch
 * distinguishes any more. Reads give the same result before and after: the child's entry for a page wins, the parent's is used
 * where the child has none, and tombstones are carried along like any other entry. */
static void drop_layer(fk_store *s, Layer *l) { /* forget l (already unlinked from the tree) and free it */
  Layer *last = s->layers[--s->nlayers];
  s->layers[l->slot] = last;
  last->slot = l->slot;
  layer_free(l);
}
static void set_head(fk_store *s, uint32_t b, Layer *l) {
  if (s->br[b].head) s->br[b].head->nheads--;
  s->br[b].head = l;
  if (l) l->nheads++;
}
/* Fold P (nheads == 0, exactly one child) into its child. Returns the layer that survives, or NULL if it ran out of memory. A
 * failed fold leaves a valid tree: every entry copied so far is one the reader would have found anyway. */
static Layer *fold_into_kid(fk_store *s, Layer *P) {
  Layer *C = P->kid;
  if (C->count < P->count) {
    /* C's entries go into P, and P takes C's place: C's children and heads move to P. */
    for (uint32_t i = 0; i < C->cap; i++) {
      if (C->key[i] == KEY_EMPTY) continue;
#ifdef FK_BUG_FOLD_PARENT_WINS
      if (layer_get(P, C->key[i])) continue; /* mutant: the parent's entry wins over the child's */
#endif
      if (layer_set(P, C->key[i], C->val[i]) != FK_OK) return NULL;
    }
#ifndef FK_BUG_FOLD_SIZE
    P->size = C->size; /* mutant: P keeps its own size */
#endif
    while (C->kid) {
      Layer *k = C->kid;
      layer_unlink(k);
      layer_link(k, P);
    }
#ifndef FK_BUG_FOLD_NO_RETARGET
    for (uint32_t b = 0; b < s->nb && C->nheads; b++)
      if (s->br[b].alive && s->br[b].head == C) set_head(s, b, P); /* mutant: a branch is left pointing at the freed layer */
#endif
    layer_unlink(C);
    drop_layer(s, C);
    return P;
  }
  /* P's entries go into C, where C has none, and C takes P's place under P's parent. */
  for (uint32_t i = 0; i < P->cap; i++) {
    if (P->key[i] == KEY_EMPTY) continue;
    uint32_t pg = P->key[i];
    uint64_t v = P->val[i];
#ifdef FK_BUG_FOLD_DROP_TOMB
    if (v == TOMB) continue; /* mutant: a tombstone is not carried over, so the page below shows through */
#else
    if (v == TOMB && !P->parent) continue; /* nothing below to hide */
#endif
#ifndef FK_BUG_FOLD_PARENT_WINS
    if (layer_get(C, pg)) continue; /* the child's entry wins */
#endif
    if (layer_set(C, pg, v) != FK_OK) return NULL;
  }
  Layer *G = P->parent;
  layer_unlink(C);
  layer_unlink(P);
  layer_link(C, G);
  drop_layer(s, P);
  return C;
}
/* l has just lost a head or a child: restore the invariant from l upwards. */
static void layer_settle(fk_store *s, Layer *l) {
  while (l) {
#ifdef FK_BUG_LAYER_FREE_SHARED
    if (l->nheads == 0) { /* mutant: a layer is freed when no branch is on it, even if other layers build on it */
#else
    if (l->nheads == 0 && l->nkids == 0) {
#endif
      Layer *p = l->parent;
      layer_unlink(l);
      drop_layer(s, l);
      l = p;
#ifdef FK_BUG_FOLD_SHARED
    } else if (l->nheads == 0 && l->nkids >= 1 && l->nkids <= 2) { /* mutant: also folds a layer with two children */
#elif defined(FK_BUG_LAYER_NO_FOLD)
    } else if (0) { /* mutant: single-child layers stay */
#else
    } else if (l->nheads == 0 && l->nkids == 1) {
#endif
      l = fold_into_kid(s, l);
    } else {
      break;
    }
  }
}
/* Replace a branch's metadata (len 0 clears it). On failure nothing has changed. */
static int meta_put(fk_store *s, uint32_t b, const uint8_t *m, uint32_t len) {
  uint8_t *n = NULL;
  if (len) {
    n = malloc(len);
    if (!n) return FK_ENOMEM;
    memcpy(n, m, len);
  }
  free(s->br[b].meta);
  s->br[b].meta = n;
  s->br[b].nmeta = (uint16_t)len;
  return FK_OK;
}

/* A branch is discarded: it no longer has a head. Its metadata goes with it, so that a later fork given the same id starts without any. */
static void release_branch(fk_store *s, uint32_t b) {
  Layer *h = s->br[b].head;
  s->br[b].alive = 0;
#ifndef FK_BUG_META_KEPT_ON_DISCARD
  (void)meta_put(s, b, NULL, 0); /* mutant: the next branch to get this id inherits the metadata */
#endif
  s->br[b].state = 0;
  set_head(s, b, NULL);
#ifdef FK_BUG_LAYER_NO_FREE
  return; /* mutant: nothing is freed or folded */
#endif
  if (h) layer_settle(s, h);
}

static int apply_commit(fk_store *s, uint32_t b, const PendPage *pp, uint32_t n, uint32_t new_size) {
  Layer *h = s->br[b].head;
  int rc;
  for (uint32_t pg = new_size; pg < h->size; pg++) {
    uint64_t v = layer_lookup(h, pg);
    if (v == 0 || v == TOMB) continue; /* already reads as zeros: there is nothing below to hide, so no entry is needed */
    if ((rc = layer_set(h, pg, TOMB)) != FK_OK) return rc;
  }
  for (uint32_t i = 0; i < n; i++)
    if ((rc = layer_set(h, pp[i].pgno, pp[i].off)) != FK_OK) return rc;
  h->size = new_size;
#ifndef FK_BUG_STATE_COMMIT_NOT_BUMPED
  s->br[b].state = ++s->state_ctr; /* mutant: a commit leaves the state id as it was */
#endif
  return FK_OK;
}
static int apply_fork(fk_store *s, uint32_t src, uint32_t dst) {
  Layer *f = s->br[src].head;
  Layer *stacked = NULL; /* a new head for src, undone if the fork layer itself cannot be allocated */
  if (f->count == 0 && f->parent && f->size == f->parent->size) {
    f = f->parent; /* nothing changed since the last fork: share the parent instead of stacking an empty layer */
  } else {
    stacked = layer_new(s, f);
    if (!stacked) return FK_ENOMEM;
    set_head(s, src, stacked);
  }
  Layer *d = layer_new(s, f);
  if (!d) {
    if (stacked) { /* the branch table is unchanged: src is back on the layer it had */
      set_head(s, src, stacked->parent);
      layer_unlink(stacked);
      drop_layer(s, stacked);
    }
    return FK_ENOMEM;
  }
  set_head(s, dst, d);
  s->br[dst].alive = 1;
#ifdef FK_BUG_STATE_FORK_NOT_COPIED
  s->br[dst].state = ++s->state_ctr; /* mutant: a fork gets an id of its own although it holds the same content */
#else
  s->br[dst].state = s->br[src].state;
#endif
  if (dst >= s->nb) s->nb = dst + 1;
  s->last_id = dst;
  return FK_OK;
}
/* dst takes the content of src: the same two steps as a fork, but the new head goes to an existing branch, and the layers dst was on
 * are released (folded or freed like after a discard). O(1). dst keeps its metadata. */
static int apply_reset(fk_store *s, uint32_t dst, uint32_t src) {
#ifdef FK_BUG_RESET_NO_EFFECT
  (void)dst; (void)src;
  return FK_OK; /* mutant: the branch keeps its content */
#endif
  Layer *f = s->br[src].head;
  if (f->count == 0 && f->parent && f->size == f->parent->size) {
    f = f->parent; /* nothing changed since the last fork: share the parent instead of stacking an empty layer */
  } else {
    Layer *nh = layer_new(s, f);
    if (!nh) return FK_ENOMEM;
    set_head(s, src, nh);
  }
  Layer *d = layer_new(s, f);
  if (!d) return FK_ENOMEM;
  Layer *old = s->br[dst].head;
  set_head(s, dst, d);
  if (old) layer_settle(s, old);
#ifdef FK_BUG_STATE_RESET_NOT_COPIED
  s->br[dst].state = ++s->state_ctr; /* mutant: after a reset the two branches hold the same content under different ids */
#else
  s->br[dst].state = s->br[src].state;
#endif
  return FK_OK;
}

/* ---------- log records ---------- */
static int put_rec_io(const fk_io *io, uint16_t type, const uint8_t *pay, uint32_t len, uint64_t *w_end, uint64_t *w_seq, uint32_t *w_last,
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
  if (io->pwrite(io->ctx, *w_end, buf, REC_HDR + (size_t)len) != 0) {
    free(buf);
    return FK_EIO;
  }
  if (rec_off) *rec_off = *w_end;
  *w_end += REC_HDR + (uint64_t)len;
  (*w_seq)++;
  *w_last = c;
  free(buf);
  return FK_OK;
}
static int put_rec(fk_store *s, uint16_t type, const uint8_t *pay, uint32_t len, uint64_t *w_end, uint64_t *w_seq, uint32_t *w_last,
                   uint64_t *rec_off) {
  int rc = put_rec_io(&s->io, type, pay, len, w_end, w_seq, w_last, rec_off);
  if (rc == FK_EIO) s->failed = 1;
  return rc;
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
static int marker_put(const fk_io *io, uint64_t gen, uint64_t end) {
  uint8_t m[32];
  memset(m, 0, sizeof m);
  memcpy(m, "FKMK", 4);
  put64(m + 8, gen);
  put64(m + 16, end);
  put32(m + 4, fk_crc32c(0, m + 8, 16));
  return io->pwrite(io->ctx, 512u * (1 + (unsigned)(gen & 1)), m, 32) == 0 ? FK_OK : FK_EIO;
}
static int write_marker(fk_store *s, uint64_t end, int sync) {
  if (marker_put(&s->io, s->mark_gen + 1, end) != FK_OK) return FK_EIO;
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

/* ---------- snapshots (written by compaction) ----------
 * A snapshot is the first unit of a compacted file: CPAGE* SNAP* SNAPEND. CPAGE records are the live pages, each once.
 * SNAP records carry, in chunks, a byte stream that describes the layer graph:
 *   u32 branches, u32 layers; per layer: u32 parent (or NONE), u32 size, u32 n, n x (u32 page, u64 offset of its CPAGE or TOMB);
 *   per branch: u8 alive, u32 head layer.
 * SNAPEND carries the stream length and its checksum. */
typedef struct { uint8_t *p; size_t n, cap; int err; } Buf;
static void b_put(Buf *b, const void *d, size_t n) {
  if (b->err) return;
  if (b->n + n > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->n + n) nc *= 2;
    uint8_t *q = realloc(b->p, nc);
    if (!q) { b->err = 1; return; }
    b->p = q;
    b->cap = nc;
  }
  memcpy(b->p + b->n, d, n);
  b->n += n;
}
static void b_u32(Buf *b, uint32_t v) { uint8_t t[4]; put32(t, v); b_put(b, t, 4); }
static void b_u64(Buf *b, uint64_t v) { uint8_t t[8]; put64(t, v); b_put(b, t, 8); }
typedef struct { const uint8_t *p; size_t n, i; int bad; } Rd;
static uint32_t r_u32(Rd *r) { if (r->bad || r->n - r->i < 4) { r->bad = 1; return 0; } uint32_t v = get32(r->p + r->i); r->i += 4; return v; }
static uint64_t r_u64(Rd *r) { if (r->bad || r->n - r->i < 8) { r->bad = 1; return 0; } uint64_t v = get64(r->p + r->i); r->i += 8; return v; }
static uint8_t r_u8(Rd *r) { if (r->bad || r->n - r->i < 1) { r->bad = 1; return 0; } return r->p[r->i++]; }

static const PendPage *cp_find(const PendPage *cp, uint32_t ncp, uint64_t off) {
  uint32_t lo = 0, hi = ncp;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (cp[mid].off < off) lo = mid + 1; else hi = mid;
  }
  return lo < ncp && cp[lo].off == off ? &cp[lo] : NULL;
}

/* Replace the whole in-memory state with the snapshot. Builds the new layers on the side: on failure nothing has changed. */
static int snap_load(fk_store *s, const uint8_t *st, size_t n, const PendPage *cp, uint32_t ncp, uint32_t version) {
  Rd r = { st, n, 0, 0 };
  uint32_t nb = r_u32(&r), nl = r_u32(&r);
  if (r.bad || nb == 0 || nb > FK_MAX_BRANCHES || nl == 0 || nl > n / 12) return FK_ECORRUPT;
  Layer **L = calloc(nl, sizeof *L);
  if (!L) return FK_ENOMEM;
  int rc = FK_OK;
  for (uint32_t i = 0; i < nl && rc == FK_OK; i++) {
    uint32_t par = r_u32(&r), size = r_u32(&r), cnt = r_u32(&r);
    if (r.bad || (par != NONE && par >= i) || size > FK_MAX_PAGES || (uint64_t)cnt * 12 > n - r.i) { rc = FK_ECORRUPT; break; }
    Layer *l = calloc(1, sizeof *l);
    if (!l) { rc = FK_ENOMEM; break; }
    L[i] = l;
    l->slot = i;
    layer_link(l, par == NONE ? NULL : L[par]);
    l->size = size;
    if (cnt && (rc = layer_reserve(l, cnt)) != FK_OK) break;
    int64_t prev = -1;
    for (uint32_t c = 0; c < cnt && rc == FK_OK; c++) {
      uint32_t pg = r_u32(&r);
      uint64_t off = r_u64(&r);
      if (r.bad || (int64_t)pg <= prev || pg >= FK_MAX_PAGES) { rc = FK_ECORRUPT; break; }
      prev = pg;
      if (off != TOMB) { /* it must be one of this snapshot's page copies, and a copy of this page */
        const PendPage *q = cp_find(cp, ncp, off);
        if (!q || q->pgno != pg) { rc = FK_ECORRUPT; break; }
      }
      rc = layer_set(l, pg, off);
    }
  }
  Branch br[FK_MAX_BRANCHES];
  memset(br, 0, sizeof br);
  for (uint32_t b = 0; b < nb && rc == FK_OK; b++) {
    uint8_t alive = r_u8(&r);
    uint32_t head = r_u32(&r);
    if (r.bad || alive > 1 || (alive && head >= nl) || (b == 0 && !alive)) { rc = FK_ECORRUPT; break; }
    br[b].alive = alive;
    br[b].head = alive ? L[head] : NULL;
  }
  uint32_t last = nb > 1 ? nb - 1 : 0; /* version 2 snapshots have no cursor; there the latest fork always had the highest id */
  if (rc == FK_OK && version >= 3) {
    last = r_u32(&r);
    if (r.bad || last >= nb) rc = FK_ECORRUPT;
  }
  uint64_t ctr = 0;
  if (rc == FK_OK && version >= 4) {
    /* version 4: the state counter, then per branch id its state id and metadata (dead ids: zero and nothing) */
    ctr = r_u64(&r);
    for (uint32_t b = 0; b < nb && rc == FK_OK; b++) {
      uint64_t st = r_u64(&r);
      uint32_t ml = r_u32(&r);
      if (r.bad || ml > FK_META_MAX || n - r.i < ml || (!br[b].alive && (st || ml)) || st > ctr) { rc = FK_ECORRUPT; break; }
      if (ml) {
        br[b].meta = malloc(ml);
        if (!br[b].meta) { rc = FK_ENOMEM; break; }
        memcpy(br[b].meta, r.p + r.i, ml);
        br[b].nmeta = (uint16_t)ml;
      }
      r.i += ml;
      br[b].state = st;
    }
  } else if (rc == FK_OK) {
    /* older snapshots know nothing of state ids: give every live branch an id of its own, so that no two are ever taken for equal */
    for (uint32_t b = 0; b < nb; b++)
      if (br[b].alive) br[b].state = ++ctr;
#ifdef FK_BUG_V3_SNAP_STATES_EQUAL
    for (uint32_t b = 0; b < nb; b++) br[b].state = 0; /* mutant: they all get the same one */
#endif
  }
  if (rc == FK_OK && r.i != n) rc = FK_ECORRUPT;
  for (uint32_t b = 0; b < nb && rc == FK_OK; b++)
    if (br[b].alive) br[b].head->nheads++; /* the new layers start with no heads: count them */
  if (rc != FK_OK) {
    for (uint32_t b = 0; b < FK_MAX_BRANCHES; b++) free(br[b].meta);
    for (uint32_t i = 0; i < nl; i++) layer_free(L[i]);
    free(L);
    return rc;
  }
  for (size_t i = 0; i < s->nlayers; i++) layer_free(s->layers[i]);
  free(s->layers);
  s->layers = L;
  s->nlayers = s->caplayers = nl;
  for (uint32_t b = 0; b < FK_MAX_BRANCHES; b++) free(s->br[b].meta);
  memcpy(s->br, br, sizeof br);
#ifdef FK_BUG_STATE_CTR_NOT_RESTORED
  s->state_ctr = 0; /* mutant: the counter starts again, so a state id can be handed out twice */
#else
  s->state_ctr = ctr;
#endif
  s->nb = nb;
#ifdef FK_BUG_ID_NO_CURSOR
  s->last_id = 0; /* mutant: the allocation cursor is not restored from a snapshot */
#else
  s->last_id = last;
#endif
  return FK_OK;
}

/* ---------- open and recovery ---------- */
static int write_header(const fk_io *io, int sync) {
  uint8_t h[HDR_SZ];
  memset(h, 0, sizeof h);
  memcpy(h, "FORKDBST", 8);
  put32(h + 8, FK_VERSION);
  put32(h + 12, FK_PAGE);
  put32(h + 16, fk_crc32c(0, h, 16));
  if (io->truncate(io->ctx, 0) != 0) return FK_EIO;
  if (io->pwrite(io->ctx, 0, h, sizeof h) != 0) return FK_EIO;
  if (sync && io->fsync(io->ctx) != 0) return FK_EIO;
  return FK_OK;
}
static uint32_t header_ver(const uint8_t *h) {
  uint32_t v = get32(h + 8);
  if (memcmp(h, "FORKDBST", 8) != 0 || (v < 1 || v > FK_VERSION) || get32(h + 12) != FK_PAGE || get32(h + 16) != fk_crc32c(0, h, 16)) return 0;
  return v;
}

static int scan(fk_store *s, int64_t fsize, uint32_t version, int *had_snapshot) {
  uint64_t off = HDR_SZ, c_end = HDR_SZ, seq = 1, c_seq = 1;
  uint32_t last = 0, c_last = 0, npend = 0, cpend = 0, pbranch = 0;
  PendPage *pend = NULL, *cp = NULL;
  if (had_snapshot) *had_snapshot = 0;
  uint32_t ncp = 0, ccp = 0;
  uint8_t *stream = NULL;
  size_t slen = 0, scap = 0;
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

    /* Snapshot records are only valid as the first unit of a version 2 file; ordinary records only outside one. */
    int snap_type = type == T_CPAGE || type == T_SNAP || type == T_SNAPEND;
    if (snap_type ? (version < 2 || c_end != HDR_SZ || npend) : (ncp || slen)) break;

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
      if (b >= s->nb || !s->br[b].alive || np != npend || (npend && b != pbranch) || nsize > FK_MAX_PAGES) break;
      int bad = 0;
      for (uint32_t i = 0; i < npend; i++) if (pend[i].pgno >= nsize) bad = 1;
      if (bad) break;
      if ((rc = apply_commit(s, b, pend, npend, nsize)) != FK_OK) break;
      npend = 0;
      unit = 1;
    } else if (type == T_FORK) {
      if (len != 8 || npend) break;
      uint32_t src = get32(pay), dst = get32(pay + 4);
      if (src >= s->nb || !s->br[src].alive || dst >= FK_MAX_BRANCHES) break;
#ifdef FK_BUG_ID_REPLAY_STRICT
      if (dst != s->nb) break; /* mutant: recovery only knows the old rule (a new id, never a reused one) */
#else
      if (s->version >= 3 ? !(dst != 0 && (dst == s->nb || (dst < s->nb && !s->br[dst].alive))) : dst != s->nb) break;
#endif
      if ((rc = apply_fork(s, src, dst)) != FK_OK) break;
      unit = 1;
    } else if (type == T_DISCARD) {
      if (len != 4 || npend) break;
      uint32_t b = get32(pay);
      if (b == 0 || b >= s->nb || !s->br[b].alive) break;
      release_branch(s, b);
      unit = 1;
    } else if (type == T_FORKM) {
      if (version < 4 || len < 12 || npend) break;
      uint32_t src = get32(pay), dst = get32(pay + 4), ml = get32(pay + 8);
      if (ml > FK_META_MAX || len != 12 + ml) break;
      if (src >= s->nb || !s->br[src].alive || dst >= FK_MAX_BRANCHES) break;
      if (!(dst != 0 && (dst == s->nb || (dst < s->nb && !s->br[dst].alive)))) break;
      if ((rc = apply_fork(s, src, dst)) != FK_OK) break;
#ifndef FK_BUG_FORKM_REPLAY_NO_META
      if ((rc = meta_put(s, dst, pay + 12, ml)) != FK_OK) break; /* mutant: recovery forgets the metadata a fork was given */
#endif
      unit = 1;
    } else if (type == T_RESET) {
      if (version < 4 || len != 8 || npend) break;
      uint32_t dst = get32(pay), src = get32(pay + 4);
      if (dst == src || dst >= s->nb || src >= s->nb || !s->br[dst].alive || !s->br[src].alive) break;
      if ((rc = apply_reset(s, dst, src)) != FK_OK) break;
      unit = 1;
    } else if (type == T_META) {
      if (version < 4 || len < 8 || npend) break;
      uint32_t b = get32(pay), ml = get32(pay + 4);
      if (ml > FK_META_MAX || len != 8 + ml || b >= s->nb || !s->br[b].alive) break;
      if ((rc = meta_put(s, b, pay + 8, ml)) != FK_OK) break;
      unit = 1;
    } else if (type == T_CPAGE) {
      if (len != PAGE_PAYLOAD || slen || get32(pay) != NONE || get32(pay + 4) >= FK_MAX_PAGES) break;
      if (ncp == ccp) {
        uint32_t nc = ccp ? ccp * 2 : 64;
        PendPage *n = realloc(cp, nc * sizeof *n);
        if (!n) { rc = FK_ENOMEM; break; }
        cp = n;
        ccp = nc;
      }
      cp[ncp].pgno = get32(pay + 4);
      cp[ncp].off = off;
      cp[ncp].data = NULL;
      ncp++;
    } else if (type == T_SNAP) {
      if (len == 0 || slen + len > FK_MAX_SNAP) break;
      if (slen + len > scap) {
        size_t nc = scap ? scap * 2 : 8192;
        while (nc < slen + len) nc *= 2;
        uint8_t *n = realloc(stream, nc);
        if (!n) { rc = FK_ENOMEM; break; }
        stream = n;
        scap = nc;
      }
      memcpy(stream + slen, pay, len);
      slen += len;
    } else if (type == T_SNAPEND) {
      if (len != 12 || !slen || get64(pay) != slen || get32(pay + 8) != fk_crc32c(0, stream, slen)) break;
      int r2 = snap_load(s, stream, slen, cp, ncp, version);
      if (r2 == FK_ECORRUPT) break;
      if (r2 != FK_OK) { rc = r2; break; }
      if (had_snapshot) *had_snapshot = 1;
      ncp = 0;
      slen = 0;
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
  free(cp);
  free(stream);
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
#ifndef FK_BUG_COMPACT_NO_CLEANUP
  /* A temporary file left by a compaction that did not finish is never trusted: delete it. */
  if (io->sibling_remove) io->sibling_remove(io->ctx, NULL);
#endif
  int64_t size = io->size(io->ctx);
  if (size < 0) { free(s); return FK_EIO; }
  uint8_t h[HDR_SZ];
  int fresh = 0;
  uint32_t version = FK_VERSION;
  if (size < (int64_t)HDR_SZ) {
    fresh = 1; /* new file, or a creation that was cut short (records are only written after the header is durable) */
  } else {
    if (io->pread(io->ctx, 0, h, HDR_SZ) != (int)HDR_SZ) { free(s); return FK_EIO; }
    version = header_ver(h);
    if (!version) {
      if (size == (int64_t)HDR_SZ) fresh = 1; /* header sector never landed: still a creation in progress */
      else { free(s); return FK_ECORRUPT; }
    }
  }
  if (fresh) {
    version = FK_VERSION;
    if ((rc = write_header(&s->io, 1)) != FK_OK) { free(s); return rc; }
    size = HDR_SZ;
  }
  if (fresh) { s->mark_end = HDR_SZ; s->mark_gen = 0; } else read_marker(s);
  s->version = version;
  Layer *main_layer = layer_new(s, NULL);
  if (!main_layer) { fk_close(s); return FK_ENOMEM; }
  set_head(s, 0, main_layer);
  s->br[0].alive = 1;
  s->nb = 1;
  int had_snapshot = 0;
  if ((rc = scan(s, size, version, &had_snapshot)) != FK_OK) { fk_close(s); return rc; }
  /* Open-time checkpoint: if the log was replayed from records (not a snapshot),
   * compact to make the next open fast. Best-effort: never fails the open. */
  if (!had_snapshot && io->sibling_create && io->sibling_rename) {
    int did = 0;
    fk_compact_if_needed(s, FK_CHECKPOINT_MIN_BYTES, &did);
  }
  *out = s;
  return FK_OK;
}

void fk_close(fk_store *s) {
  if (!s) return;
  if (!s->failed && s->end > s->mark_end) (void)write_marker(s, s->end, 1);
  for (size_t i = 0; i < s->nlayers; i++) layer_free(s->layers[i]);
  free(s->layers);
  for (uint32_t i = 0; i < FK_MAX_BRANCHES; i++) free(s->br[i].meta);
  for (uint32_t i = 0; i < s->npp; i++) free(s->pp[i].data);
  free(s->pp);
  free(s);
}

/* ---------- queries ---------- */
uint32_t fk_branch_count(const fk_store *s) { return s->nb; }
int fk_branch_alive(const fk_store *s, uint32_t b) { return b < s->nb && s->br[b].alive; }
int fk_is_failed(const fk_store *s) { return s->failed; }
uint32_t fk_layer_count(const fk_store *s) { return (uint32_t)s->nlayers; }
int fk_branch_depth(const fk_store *s, uint32_t b, uint32_t *depth) {
  if (!fk_branch_alive(s, b)) return FK_ENOENT;
  uint32_t d = 0;
  for (const Layer *l = s->br[b].head; l; l = l->parent) d++;
  *depth = d;
  return FK_OK;
}
uint64_t fk_index_bytes(const fk_store *s) {
  uint64_t t = (uint64_t)s->caplayers * sizeof(Layer *);
  for (size_t i = 0; i < s->nlayers; i++) t += sizeof(Layer) + (uint64_t)s->layers[i]->cap * (sizeof(uint32_t) + sizeof(uint64_t));
  return t;
}
int fk_size(const fk_store *s, uint32_t b, uint32_t *pages) {
  if (!fk_branch_alive(s, b)) return FK_ENOENT;
  *pages = s->br[b].head->size;
  return FK_OK;
}
/* Read and check one page record (PAGE or compaction's CPAGE) into rec (REC_PAGE_SZ bytes). */
static int read_pagerec(const fk_io *io, uint64_t off, uint32_t pgno, uint8_t *rec) {
  if (io->pread(io->ctx, off, rec, REC_PAGE_SZ) != (int)REC_PAGE_SZ) return FK_ECORRUPT;
  uint16_t type = get16(rec + 4);
  uint8_t hc[REC_HDR];
  memcpy(hc, rec, REC_HDR);
  put32(hc + 24, 0);
  uint32_t c = fk_crc32c(fk_crc32c(0, hc, REC_HDR), rec + REC_HDR, PAGE_PAYLOAD);
  if (get32(rec) != MAGIC || (type != T_PAGE && type != T_CPAGE) || get32(rec + 16) != PAGE_PAYLOAD || c != get32(rec + 24) ||
      get32(rec + REC_HDR + 4) != pgno)
    return FK_ECORRUPT;
  return FK_OK;
}
int fk_read(fk_store *s, uint32_t b, uint32_t pgno, void *buf) {
  if (s->failed) return FK_EFAILED;
  if (!fk_branch_alive(s, b)) return FK_ENOENT;
  if (pgno >= s->br[b].head->size) return FK_ENOENT;
  uint64_t off = layer_lookup(s->br[b].head, pgno);
  if (off == 0 || off == TOMB) { memset(buf, 0, FK_PAGE); return FK_OK; }
  uint8_t rec[REC_PAGE_SZ];
  int rc = read_pagerec(&s->io, off, pgno, rec);
  if (rc != FK_OK) return rc;
  memcpy(buf, rec + REC_HDR + 8, FK_PAGE);
  return FK_OK;
}

/* ---------- writes ---------- */
static void commit_tail(fk_store *s, uint64_t end, uint64_t seq, uint32_t last) { s->end = end; s->seq = seq; s->last_crc = last; }

/* Which id a fork gets. Ids of discarded branches are reused, **next fit**: the search starts just after the id the latest fork was
 * given and wraps around (skipping 0), so a just-discarded id is the last one to come back, not the first. A caller that keeps a
 * stale id therefore has to survive a whole round of the id space before it can be fooled. Version 1 and 2 files never reuse ids
 * (an older program would not understand the log). The rule depends only on the alive flags and the cursor, both of which a
 * snapshot keeps, so replaying the log or loading a snapshot reproduces it. */
static int alloc_id(const fk_store *s, uint32_t *id) {
  if (s->version < 3) {
    if (s->nb >= FK_MAX_BRANCHES) return FK_ELIMIT;
    *id = s->nb;
    return FK_OK;
  }
  uint32_t last = s->last_id;
  for (uint32_t k = 1; k < FK_MAX_BRANCHES; k++) {
    uint32_t cand = last + k;
    if (cand >= FK_MAX_BRANCHES) cand -= FK_MAX_BRANCHES - 1; /* wrap, skipping id 0 */
    int free_ = cand >= s->nb || !s->br[cand].alive;
#ifdef FK_BUG_ID_REUSE_ALIVE
    if (k == 1) free_ = 1; /* mutant: the first candidate is taken even if its branch is alive */
#endif
    if (free_) { *id = cand; return FK_OK; }
  }
  return FK_ELIMIT;
}

int fk_fork(fk_store *s, uint32_t src, uint32_t *dst) {
  if (s->failed) return FK_EFAILED;
  if (s->txn_open) return FK_EBUSY;
  if (!fk_branch_alive(s, src)) return FK_EINVAL;
  uint32_t id = 0;
  int arc = alloc_id(s, &id);
  if (arc != FK_OK) return arc;
  uint8_t pay[8];
  put32(pay, src);
  put32(pay + 4, id);
  uint64_t old_end = s->end, e = old_end, q = s->seq, old_seq = q;
  uint32_t old_crc = s->last_crc, l = old_crc;
  int rc = put_rec(s, T_FORK, pay, 8, &e, &q, &l, NULL);
  if (rc == FK_OK) rc = do_sync(s);
  if (rc != FK_OK) return rc;
  commit_tail(s, e, q, l);
  if ((rc = apply_fork(s, src, id)) != FK_OK) {
    /* The record is durable but the branch table was not changed (apply_fork undoes a partial layer).
     * Cut the record off so a retry cannot collide with it. If the cut fails, memory and the log disagree. */
    if (!s->io.truncate || s->io.truncate(s->io.ctx, old_end) != 0 || s->io.fsync(s->io.ctx) != 0) {
      s->failed = 1;
      return rc;
    }
    s->end = old_end;
    s->seq = old_seq;
    s->last_crc = old_crc;
    return rc;
  }
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
  release_branch(s, b);
  maybe_mark(s);
  return FK_OK;
}

/* ---------- version 4: state ids, metadata, reset ---------- */
int fk_version(const fk_store *s) { return (int)s->version; }
int fk_state(const fk_store *s, uint32_t b, uint64_t *st) {
  if (s->failed) return FK_EFAILED;
  if (!fk_branch_alive(s, b) || !st) return FK_ENOENT;
  *st = s->br[b].state;
  return FK_OK;
}
int fk_meta_get(const fk_store *s, uint32_t b, void *buf, size_t cap, size_t *len) {
  if (s->failed) return FK_EFAILED;
  if (!fk_branch_alive(s, b)) return FK_ENOENT;
  size_t n = s->br[b].nmeta;
  if (len) *len = n;
  if (buf && cap && n) memcpy(buf, s->br[b].meta, n < cap ? n : cap);
  return FK_OK;
}
/* One record, one fsync: the append that fork, discard, reset and meta all use. */
static int append_unit(fk_store *s, uint16_t type, const uint8_t *pay, uint32_t len) {
  uint64_t e = s->end, q = s->seq;
  uint32_t l = s->last_crc;
  int rc = put_rec(s, type, pay, len, &e, &q, &l, NULL);
  if (rc == FK_OK) rc = do_sync(s);
  if (rc != FK_OK) return rc;
  commit_tail(s, e, q, l);
  return FK_OK;
}
int fk_reset(fk_store *s, uint32_t dst, uint32_t src) {
  if (s->failed) return FK_EFAILED;
  if (s->txn_open) return FK_EBUSY;
  if (s->version < 4) return FK_EVERSION;
  if (dst == src || !fk_branch_alive(s, dst) || !fk_branch_alive(s, src)) return FK_EINVAL;
  uint8_t pay[8];
  put32(pay, dst);
  put32(pay + 4, src);
  int rc = append_unit(s, T_RESET, pay, 8);
  if (rc != FK_OK) return rc;
  if ((rc = apply_reset(s, dst, src)) != FK_OK) { s->failed = 1; return rc; }
  maybe_mark(s);
  return FK_OK;
}
int fk_meta_set(fk_store *s, uint32_t b, const void *meta, size_t len) {
  if (s->failed) return FK_EFAILED;
  if (s->txn_open) return FK_EBUSY;
  if (s->version < 4) return FK_EVERSION;
  if (!fk_branch_alive(s, b) || len > FK_META_MAX || (len && !meta)) return FK_EINVAL;
  uint8_t pay[8 + FK_META_MAX];
  put32(pay, b);
  put32(pay + 4, (uint32_t)len);
  if (len) memcpy(pay + 8, meta, len);
  int rc = append_unit(s, T_META, pay, 8 + (uint32_t)len);
  if (rc != FK_OK) return rc;
  if ((rc = meta_put(s, b, pay + 8, (uint32_t)len)) != FK_OK) { s->failed = 1; return rc; }
  maybe_mark(s);
  return FK_OK;
}
int fk_fork_meta(fk_store *s, uint32_t src, const void *meta, size_t len, uint32_t *dst) {
  if (s->failed) return FK_EFAILED;
  if (s->txn_open) return FK_EBUSY;
  if (s->version < 4) return FK_EVERSION;
  if (!fk_branch_alive(s, src) || len > FK_META_MAX || (len && !meta)) return FK_EINVAL;
  uint32_t id = 0;
  int rc = alloc_id(s, &id);
  if (rc != FK_OK) return rc;
  uint8_t pay[12 + FK_META_MAX];
  put32(pay, src);
  put32(pay + 4, id);
  put32(pay + 8, (uint32_t)len);
  if (len) memcpy(pay + 12, meta, len);
  if ((rc = append_unit(s, T_FORKM, pay, 12 + (uint32_t)len)) != FK_OK) return rc;
  if ((rc = apply_fork(s, src, id)) != FK_OK) { s->failed = 1; return rc; }
  if ((rc = meta_put(s, id, pay + 12, (uint32_t)len)) != FK_OK) { s->failed = 1; return rc; }
  maybe_mark(s);
  *dst = id;
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
static int txn_push(fk_store *s, uint32_t pgno, uint8_t *data) { /* the pending list takes data on FK_OK */
  if (!s->txn_open) return FK_EINVAL;
  if (s->npp == s->cpp) {
    uint32_t nc = s->cpp ? s->cpp * 2 : 8;
    PendPage *n = realloc(s->pp, nc * sizeof *n);
    if (!n) return FK_ENOMEM;
    s->pp = n;
    s->cpp = nc;
  }
  s->pp[s->npp].pgno = pgno;
  s->pp[s->npp].off = 0;
  s->pp[s->npp].data = data;
  s->npp++;
  return FK_OK;
}
int fk_txn_write(fk_store *s, uint32_t pgno, const void *page) {
  if (!s->txn_open) return FK_EINVAL;
  uint8_t *d = malloc(FK_PAGE);
  if (!d) return FK_ENOMEM;
  memcpy(d, page, FK_PAGE);
  int rc = txn_push(s, pgno, d);
  if (rc != FK_OK) free(d);
  return rc;
}
int fk_txn_write_owned(fk_store *s, uint32_t pgno, void *page) {
  if (!page) return FK_EINVAL;
  return txn_push(s, pgno, page); /* on an error the caller still owns page */
}
void fk_txn_abort(fk_store *s) {
  for (uint32_t i = 0; i < s->npp; i++) free(s->pp[i].data);
  s->npp = 0;
  s->txn_open = 0;
}
int fk_txn_commit(fk_store *s, uint32_t new_size) {
  if (s->failed) return FK_EFAILED;
  if (!s->txn_open) return FK_EINVAL;
  if (new_size > FK_MAX_PAGES) return FK_EINVAL;
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

/* ---------- compaction ---------- */
typedef struct { uint32_t parent, size, n; uint32_t *pg; uint64_t *off; } PL; /* a planned layer; off = offsets in the OLD file */
typedef struct { uint64_t off; uint32_t pg; } UP;                            /* a live page: its record in the old file */
typedef struct {
  PL *l;
  uint32_t nl;
  uint32_t head[FK_MAX_BRANCHES];
  UP *u; /* live pages, ascending by old offset; the i-th goes to offset HDR_SZ + i * REC_PAGE_SZ of the new file */
  size_t nu;
} Plan;

static void plan_free(Plan *P) {
  for (uint32_t i = 0; i < P->nl; i++) { free(P->l[i].pg); free(P->l[i].off); }
  free(P->l);
  free(P->u);
  memset(P, 0, sizeof *P);
}
static int up_cmp(const void *a, const void *b) {
  const UP *x = a, *y = b;
  return x->off < y->off ? -1 : x->off > y->off;
}

typedef struct { uint32_t pg, prio; uint64_t off; } PE; /* one entry of a layer, with the distance of its layer from the kept one */
static int pe_cmp(const void *a, const void *b) {
  const PE *x = a, *y = b;
  if (x->pg != y->pg) return x->pg < y->pg ? -1 : 1;
  return x->prio < y->prio ? -1 : x->prio > y->prio;
}

/* What is live: for every live branch walk its layer chain. A frozen layer with exactly one child and no branch of its own is
 * folded into that child (this is the flattening); other layers are kept. Tombstones only matter if there is a layer below to
 * hide, so a layer without a parent drops them. */
static int plan_build(fk_store *s, Plan *P) {
  memset(P, 0, sizeof *P);
  for (size_t i = 0; i < s->nlayers; i++) { Layer *l = s->layers[i]; l->seen = 0; l->kids = l->heads = 0; l->idx = NONE; }
  for (uint32_t b = 0; b < s->nb; b++) {
    if (!s->br[b].alive) continue;
    Layer *h = s->br[b].head;
    h->heads++;
    for (Layer *l = h; l && !l->seen; l = l->parent) l->seen = 1;
  }
  for (size_t i = 0; i < s->nlayers; i++) if (s->layers[i]->seen && s->layers[i]->parent) s->layers[i]->parent->kids++;
#define KEPT(l) ((l)->heads > 0 || (l)->kids != 1)
  /* Number the kept layers so that a layer's kept ancestor always has the lower number (the snapshot format and its loader
   * require it). The array of layers is not in that order (dropping a layer moves the last one into its slot), so walk the tree
   * from its root, parents before children, with an explicit stack. */
  uint32_t nk = 0;
  {
    Layer **stack = malloc((s->nlayers ? s->nlayers : 1) * sizeof *stack);
    if (!stack) return FK_ENOMEM;
    for (size_t i = 0; i < s->nlayers; i++) {
      if (s->layers[i]->parent) continue; /* a root */
      size_t sp = 0;
      stack[sp++] = s->layers[i];
      while (sp) {
        Layer *l = stack[--sp];
        if (l->seen && KEPT(l)) l->idx = nk++;
        for (Layer *k = l->kid; k; k = k->sib) stack[sp++] = k;
      }
    }
    free(stack);
    for (size_t i = 0; i < s->nlayers; i++)
      if (s->layers[i]->seen && KEPT(s->layers[i]) && s->layers[i]->idx == NONE) return FK_ECORRUPT; /* not reachable from a root: cannot happen */
  }
  P->l = calloc(nk ? nk : 1, sizeof *P->l);
  if (!P->l) return FK_ENOMEM;
  P->nl = nk;
  size_t total = 0;
  for (size_t i = 0; i < s->nlayers; i++) {
    Layer *K = s->layers[i];
    if (!K->seen || !KEPT(K)) continue;
    Layer *EP = K->parent; /* the nearest kept ancestor */
    while (EP && !KEPT(EP)) EP = EP->parent;
    /* K and the folded layers between K and EP, nearest first: the nearest layer's entry for a page wins. */
    size_t cnt = 0;
    for (Layer *l = K; l != EP; l = l->parent) cnt += l->count;
    PE *pe = malloc((cnt ? cnt : 1) * sizeof *pe);
    if (!pe) return FK_ENOMEM;
    size_t m = 0;
    uint32_t prio = 0;
    for (Layer *l = K; l != EP; l = l->parent, prio++) {
      for (uint32_t i = 0; i < l->cap; i++)
        if (l->key[i] != KEY_EMPTY) { pe[m].pg = l->key[i]; pe[m].prio = prio; pe[m].off = l->val[i]; m++; }
    }
    qsort(pe, m, sizeof *pe, pe_cmp); /* by page, nearest layer first */
    size_t n = 0;
    int64_t prev_pg = -1;
    for (size_t i = 0; i < m; i++) {
      if ((int64_t)pe[i].pg == prev_pg) continue; /* a farther layer's entry for the same page */
      prev_pg = pe[i].pg;
#ifdef FK_BUG_COMPACT_DROP_TOMB
      if (pe[i].off == TOMB) continue; /* mutant: tombstones are dropped even where they hide a lower layer */
#else
      if (pe[i].off == TOMB && !EP) continue; /* nothing below to hide */
#endif
      pe[n++] = pe[i];
    }
    PL *pl = &P->l[K->idx];
    pl->parent = EP ? EP->idx : NONE;
    pl->size = K->size;
    pl->n = (uint32_t)n;
    pl->pg = calloc(n ? n : 1, sizeof *pl->pg);
    pl->off = calloc(n ? n : 1, sizeof *pl->off);
    if (!pl->pg || !pl->off) { free(pe); return FK_ENOMEM; }
    for (size_t i = 0; i < n; i++) {
      pl->pg[i] = pe[i].pg;
      pl->off[i] = pe[i].off;
      if (pe[i].off != TOMB) total++;
    }
    free(pe);
  }
  for (uint32_t b = 0; b < s->nb; b++) P->head[b] = s->br[b].alive ? s->br[b].head->idx : NONE;
  P->u = malloc((total ? total : 1) * sizeof *P->u);
  if (!P->u) return FK_ENOMEM;
  size_t m = 0;
  for (uint32_t i = 0; i < P->nl; i++)
    for (uint32_t k = 0; k < P->l[i].n; k++)
      if (P->l[i].off[k] != TOMB) { P->u[m].off = P->l[i].off[k]; P->u[m].pg = P->l[i].pg[k]; m++; }
  qsort(P->u, m, sizeof *P->u, up_cmp);
  size_t w = 0;
  for (size_t i = 0; i < m; i++) {
    if (w && P->u[w - 1].off == P->u[i].off) {
      if (P->u[w - 1].pg != P->u[i].pg) return FK_ECORRUPT;
      continue;
    }
    P->u[w++] = P->u[i];
  }
  P->nu = w;
  return FK_OK;
}
static int new_off(const Plan *P, uint64_t old, uint64_t *out) {
  size_t lo = 0, hi = P->nu;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (P->u[mid].off < old) lo = mid + 1; else hi = mid;
  }
  if (lo >= P->nu || P->u[lo].off != old) return FK_ECORRUPT;
  *out = HDR_SZ + (uint64_t)lo * REC_PAGE_SZ;
  return FK_OK;
}
static size_t plan_stream_len(const fk_store *s, const Plan *P, uint32_t nb) {
  size_t n = 8 + 5 * (size_t)nb + 4; /* header counts, one entry per branch id, the allocation cursor */
  n += 8 + 12 * (size_t)nb;            /* version 4: the state counter, and per branch id a state id and a metadata length */
  for (uint32_t i = 0; i < nb; i++) if (P->head[i] != NONE) n += s->br[i].nmeta;
  for (uint32_t i = 0; i < P->nl; i++) n += 12 + 12 * (size_t)P->l[i].n;
  return n;
}
static uint64_t plan_live_bytes(const fk_store *s, const Plan *P, uint32_t nb) {
  size_t sl = plan_stream_len(s, P, nb);
  size_t chunks = (sl + PAGE_PAYLOAD - 1) / PAGE_PAYLOAD;
  return HDR_SZ + (uint64_t)P->nu * REC_PAGE_SZ + sl + chunks * REC_HDR + REC_HDR + 12;
}
static int plan_stream(const fk_store *s, const Plan *P, Buf *b) {
  b_u32(b, s->nb);
  b_u32(b, P->nl);
  for (uint32_t i = 0; i < P->nl; i++) {
    const PL *l = &P->l[i];
    b_u32(b, l->parent);
    b_u32(b, l->size);
    b_u32(b, l->n);
    for (uint32_t k = 0; k < l->n; k++) {
      uint64_t o = TOMB;
      if (l->off[k] != TOMB) { int rc = new_off(P, l->off[k], &o); if (rc) return rc; }
      b_u32(b, l->pg[k]);
      b_u64(b, o);
    }
  }
  for (uint32_t i = 0; i < s->nb; i++) {
    uint8_t a = P->head[i] != NONE;
    b_put(b, &a, 1);
    b_u32(b, a ? P->head[i] : 0);
  }
  b_u32(b, s->last_id); /* version 3: the allocation cursor */
  b_u64(b, s->state_ctr); /* version 4 */
  for (uint32_t i = 0; i < s->nb; i++) {
    int alive = P->head[i] != NONE;
    uint32_t ml = alive ? s->br[i].nmeta : 0;
#ifdef FK_BUG_META_LOST_COMPACT
    ml = 0; /* mutant: compaction leaves the metadata out */
#endif
    b_u64(b, alive ? s->br[i].state : 0);
    b_u32(b, ml);
    if (ml) b_put(b, s->br[i].meta, ml);
  }
  return b->err ? FK_ENOMEM : FK_OK;
}

/* Write the compacted file next to the store's file and swap it in. */
static int compact_exec(fk_store *s, const Plan *P) {
  fk_io tmp;
  memset(&tmp, 0, sizeof tmp);
  Buf sb;
  memset(&sb, 0, sizeof sb);
  PendPage *cp = NULL;
  int rc, have_tmp = 0;
  uint64_t e = HDR_SZ, q = 1;
  uint32_t l = 0;
  if ((rc = plan_stream(s, P, &sb)) != FK_OK) goto fail;
  if (s->io.sibling_create(s->io.ctx, &tmp) != 0) { rc = FK_EIO; goto fail; }
  have_tmp = 1;
  /* The new file is invisible until the rename, so none of this needs to be durable yet: one fsync below covers it all. */
  if ((rc = write_header(&tmp, 0)) != FK_OK) goto fail;
  uint8_t pay[PAGE_PAYLOAD], old[REC_PAGE_SZ];
  for (size_t i = 0; i < P->nu; i++) {
#ifdef FK_BUG_COMPACT_NO_VERIFY
    rc = s->io.pread(s->io.ctx, P->u[i].off, old, REC_PAGE_SZ) == (int)REC_PAGE_SZ ? FK_OK : FK_ECORRUPT; /* mutant: copies damaged pages */
#else
    rc = read_pagerec(&s->io, P->u[i].off, P->u[i].pg, old);
#endif
    if (rc != FK_OK) goto fail; /* a damaged page aborts the compaction: it never launders damage */
    put32(pay, NONE);
    put32(pay + 4, P->u[i].pg);
    memcpy(pay + 8, old + REC_HDR + 8, FK_PAGE);
    if (e != HDR_SZ + (uint64_t)i * REC_PAGE_SZ) { rc = FK_EINVAL; goto fail; }
    if ((rc = put_rec_io(&tmp, T_CPAGE, pay, PAGE_PAYLOAD, &e, &q, &l, NULL)) != FK_OK) goto fail;
  }
  for (size_t o = 0; o < sb.n; o += PAGE_PAYLOAD) {
    size_t n = sb.n - o < PAGE_PAYLOAD ? sb.n - o : PAGE_PAYLOAD;
    if ((rc = put_rec_io(&tmp, T_SNAP, sb.p + o, (uint32_t)n, &e, &q, &l, NULL)) != FK_OK) goto fail;
  }
  uint8_t se[12];
  put64(se, sb.n);
  put32(se + 8, fk_crc32c(0, sb.p, sb.n));
  if ((rc = put_rec_io(&tmp, T_SNAPEND, se, 12, &e, &q, &l, NULL)) != FK_OK) goto fail;
  if ((rc = marker_put(&tmp, 1, e)) != FK_OK) goto fail;
#ifndef FK_BUG_COMPACT_NO_TMP_SYNC
  if (tmp.fsync(tmp.ctx) != 0) { rc = FK_EIO; goto fail; }
#endif
  /* The point of no return. A failed rename changes nothing; after it succeeds the store's file IS the new one, whether or
   * not the directory entry is durable yet. */
  if (s->io.sibling_rename(s->io.ctx, &tmp) != 0) { rc = FK_EIO; goto fail; }
  have_tmp = 0;
#ifndef FK_BUG_COMPACT_NO_DIRSYNC
  if (s->io.sync_dir(s->io.ctx) != 0) { s->failed = 1; free(sb.p); return FK_EIO; }
#endif
  /* Switch the in-memory state to the snapshot we just wrote, through the same loader recovery uses. */
  cp = malloc((P->nu ? P->nu : 1) * sizeof *cp);
  if (!cp) { s->failed = 1; free(sb.p); return FK_ENOMEM; }
  for (size_t i = 0; i < P->nu; i++) { cp[i].pgno = P->u[i].pg; cp[i].off = HDR_SZ + (uint64_t)i * REC_PAGE_SZ; cp[i].data = NULL; }
  rc = snap_load(s, sb.p, sb.n, cp, (uint32_t)P->nu, FK_VERSION);
  free(cp);
  free(sb.p);
  if (rc != FK_OK) { s->failed = 1; return rc; }
  s->version = FK_VERSION; /* the new file has a current header */
  s->end = e;
  s->seq = q;
  s->last_crc = l;
  s->mark_gen = 1;
  s->mark_end = e;
  s->since_mark = 0;
  return FK_OK;
fail:
  if (have_tmp) s->io.sibling_remove(s->io.ctx, &tmp);
  free(sb.p);
  return rc; /* the store's file was not touched: still usable */
}

static int compact_core(fk_store *s, int force, uint64_t min_bytes, int *did) {
  if (did) *did = 0;
  if (s->failed) return FK_EFAILED;
  if (s->txn_open) return FK_EBUSY;
  if (!s->io.sibling_create || !s->io.sibling_rename || !s->io.sync_dir || !s->io.sibling_remove) return FK_EINVAL;
  Plan P;
  int rc = plan_build(s, &P);
  if (rc == FK_OK && !force && (s->end < min_bytes || plan_live_bytes(s, &P, s->nb) * 2 >= s->end)) { plan_free(&P); return FK_OK; }
  if (rc == FK_OK) rc = compact_exec(s, &P);
  plan_free(&P);
  if (rc == FK_OK && did) *did = 1;
  return rc;
}
int fk_compact(fk_store *s) { return compact_core(s, 1, 0, NULL); }
int fk_compact_if_needed(fk_store *s, uint64_t min_bytes, int *did) { return compact_core(s, 0, min_bytes, did); }
int fk_stats(fk_store *s, uint64_t *file_bytes, uint64_t *live_bytes) {
  if (s->failed) return FK_EFAILED;
  Plan P;
  int rc = plan_build(s, &P);
  if (rc == FK_OK) {
    if (file_bytes) *file_bytes = s->end;
    if (live_bytes) *live_bytes = plan_live_bytes(s, &P, s->nb);
  }
  plan_free(&P);
  return rc;
}
