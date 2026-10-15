/* Recovery fuzzer: feed damaged, truncated and deliberately malformed store files to fk_open and check that every one is either refused cleanly or opens to a store that
 * behaves (no crash, no hang, no memory error or undefined behaviour under the sanitizers, sane invariants), and that a store opened from such a file and then written to
 * reopens to the same content. Deterministic: input number N of seed S is always the same bytes (--one S N file writes it out).
 *
 * The inputs start from valid stores built through the public API (empty, commits, forks and discards with id reuse, metadata and reset, a compacted file with a snapshot)
 * and are mutated at three levels: bytes (flips, truncation, zeroed ranges, splices, garbage), fields (the file header, the durable-end markers) and records, which are
 * rewritten with a VALID checksum and a repaired chain so that they get past the CRC and reach the checks behind it: record types, lengths, branch ids, page numbers and sizes,
 * and the snapshot stream (its tables are mutated, then re-split into records with the stream checksum recomputed).
 *
 * Build: see scripts/test-fuzz.sh (sanitizers on, FK_MAX_BRANCHES=64).  Run: fuzz_open [inputs [seed]]   or   fuzz_open --one seed n [file]. */
#define _GNU_SOURCE
#include "../store/fkstore.h"
#include "simdisk.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/common_interface_defs.h>
#endif

#define HDR 4096u
#define RECH 32u
#define PAGEP (8u + FK_PAGE)
enum { T_PAGE = 1, T_COMMIT = 2, T_FORK = 3, T_DISCARD = 4, T_CPAGE = 5, T_SNAP = 6, T_SNAPEND = 7, T_FORKM = 8, T_RESET = 9, T_META = 10 };

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

typedef struct { uint8_t *p; size_t n, cap; } Img;
static void img_reserve(Img *im, size_t n) {
  if (n <= im->cap) return;
  size_t c = im->cap ? im->cap : 4096;
  while (c < n) c *= 2;
  im->p = realloc(im->p, c);
  if (!im->p) { fprintf(stderr, "fuzz: out of memory\n"); exit(2); }
  im->cap = c;
}
static void img_set(Img *im, const uint8_t *p, size_t n) { img_reserve(im, n ? n : 1); if (n) memcpy(im->p, p, n); im->n = n; }
static void img_free(Img *im) { free(im->p); im->p = NULL; im->n = im->cap = 0; }
static void img_resize(Img *im, size_t n) { size_t old = im->n; img_reserve(im, n ? n : 1); if (n > old) memset(im->p + old, 0, n - old); im->n = n; }

static uint64_t splitmix(uint64_t *x) {
  uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

/* ---------------------------------------------------------------- seed images, built through the public API on a simulated disk */
static int seed_fail;
#define MUST(c) do { if (!(c)) { fprintf(stderr, "fuzz: seed construction failed at line %d: %s\n", __LINE__, #c); seed_fail = 1; } } while (0)
static void page_fill(uint8_t *pg, uint32_t tag) { for (uint32_t i = 0; i < FK_PAGE; i++) pg[i] = (uint8_t)(tag * 131u + i * 7u + (i >> 8)); }
static void commit_pages(fk_store *s, uint32_t b, uint32_t first, uint32_t n, uint32_t tag, uint32_t size) {
  uint8_t pg[FK_PAGE];
  MUST(fk_txn_begin(s, b) == FK_OK);
  for (uint32_t i = 0; i < n; i++) { page_fill(pg, tag + i); MUST(fk_txn_write(s, first + i, pg) == FK_OK); }
  MUST(fk_txn_commit(s, size) == FK_OK);
}
static void take(Img *out, SimDisk *d, fk_store *s) {
  fk_close(s);
  uint64_t n;
  const uint8_t *v = sim_visible(d, &n);
  img_set(out, v, (size_t)n);
  sim_free(d);
}
static void seed_commits(Img *out) {
  SimDisk d; sim_init(&d, NULL, 0);
  fk_io io = sim_io(&d); fk_store *s = NULL;
  MUST(fk_open(&io, &s) == FK_OK);
  commit_pages(s, 0, 0, 3, 1, 3);
  commit_pages(s, 0, 1, 2, 10, 5);
  commit_pages(s, 0, 0, 40, 20, 40);
  take(out, &d, s);
}
static void seed_branches(Img *out) {
  SimDisk d; sim_init(&d, NULL, 0);
  fk_io io = sim_io(&d); fk_store *s = NULL;
  MUST(fk_open(&io, &s) == FK_OK);
  commit_pages(s, 0, 0, 6, 1, 6);
  uint32_t a = 0, b = 0, c = 0;
  MUST(fk_fork(s, 0, &a) == FK_OK);
  commit_pages(s, a, 2, 3, 50, 8);
  MUST(fk_fork_meta(s, a, "feature-x", 9, &b) == FK_OK);
  commit_pages(s, b, 7, 2, 60, 9);
  MUST(fk_discard(s, a) == FK_OK);
  MUST(fk_fork(s, b, &c) == FK_OK); /* reuses a's id */
  MUST(fk_meta_set(s, 0, "main", 4) == FK_OK);
  MUST(fk_reset(s, c, 0) == FK_OK);
  commit_pages(s, c, 0, 2, 70, 6);
  commit_pages(s, 0, 3, 2, 80, 6);
  take(out, &d, s);
}
static void seed_snapshot(Img *out) {
  SimDisk d; sim_init(&d, NULL, 0);
  fk_io io = sim_io(&d); fk_store *s = NULL;
  MUST(fk_open(&io, &s) == FK_OK);
  commit_pages(s, 0, 0, 12, 1, 12);
  uint32_t a = 0, b = 0;
  MUST(fk_fork(s, 0, &a) == FK_OK);
  commit_pages(s, a, 3, 4, 100, 12);
  MUST(fk_fork_meta(s, a, "snapshot-branch", 15, &b) == FK_OK);
  commit_pages(s, b, 0, 2, 120, 14);
  uint32_t j = 0;
  MUST(fk_fork(s, 0, &j) == FK_OK);
  commit_pages(s, j, 0, 12, 200, 12);
  MUST(fk_discard(s, j) == FK_OK);
  MUST(fk_compact(s) == FK_OK);
  commit_pages(s, 0, 5, 2, 300, 12); /* records after the snapshot */
  take(out, &d, s);
}
static void seed_full(Img *out) { /* every branch id in use: the next fork has dst == nb == FK_MAX_BRANCHES */
  SimDisk d; sim_init(&d, NULL, 0);
  fk_io io = sim_io(&d); fk_store *s = NULL;
  MUST(fk_open(&io, &s) == FK_OK);
  commit_pages(s, 0, 0, 4, 1, 4);
  for (uint32_t i = 1; i < FK_MAX_BRANCHES; i++) {
    uint32_t x = 0;
    MUST(fk_fork(s, 0, &x) == FK_OK);
    if (i % 9 == 0) commit_pages(s, x, 0, 1, 40 + i, 4);
  }
  take(out, &d, s);
}
#define NSEEDS 5
static Img seeds[NSEEDS];
static int seed_has_snap[NSEEDS];
static void build_seeds(void) {
  Img empty = {0};
  { SimDisk d; sim_init(&d, NULL, 0); fk_io io = sim_io(&d); fk_store *s = NULL; MUST(fk_open(&io, &s) == FK_OK); take(&empty, &d, s); }
  seeds[0] = empty;
  seed_commits(&seeds[1]);
  seed_branches(&seeds[2]);
  seed_snapshot(&seeds[3]);
  seed_has_snap[3] = 1;
  seed_full(&seeds[4]);
  if (seed_fail) exit(2);
}

/* ---------------------------------------------------------------- mutators */
typedef struct { size_t off; uint32_t len; uint16_t type; } Rec;
#define MAXREC 600
static int parse_recs(const Img *im, Rec *out, int max) {
  int n = 0;
  size_t off = HDR;
  while (n < max && off + RECH <= im->n && rd32(im->p + off) == 0x31524b46u) {
    uint32_t len = rd32(im->p + off + 16);
    if (len > PAGEP * 4 || off + RECH + len > im->n) break;
    out[n].off = off; out[n].len = len; out[n].type = (uint16_t)(im->p[off + 4] | im->p[off + 5] << 8);
    n++;
    off += RECH + len;
  }
  return n;
}
static void fix_crc(Img *im, size_t off) {
  uint32_t len = rd32(im->p + off + 16);
  if (off + RECH + len > im->n) return;
  wr32(im->p + off + 24, 0);
  uint32_t c = fk_crc32c(0, im->p + off, RECH);
  c = fk_crc32c(c, im->p + off + RECH, len);
  wr32(im->p + off + 24, c);
}
/* Make records from index i on a valid chain again: each one's prev is the crc of the one before, with the crc recomputed. */
static void repair_chain(Img *im, int i) {
  Rec r[MAXREC];
  int n = parse_recs(im, r, MAXREC);
  if (i < 0 || i >= n) return;
  fix_crc(im, r[i].off);
  for (int j = i + 1; j < n; j++) {
    wr32(im->p + r[j].off + 20, rd32(im->p + r[j - 1].off + 24));
    fix_crc(im, r[j].off);
  }
}
static void wipe_markers(Img *im) { if (im->n >= 1056) memset(im->p + 512, 0, 544); }
static uint32_t interesting(Rng *r) {
  static const uint32_t v[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 65, 255, 256, 1023, 1024, 4095, 4096, FK_MAX_BRANCHES - 1, FK_MAX_BRANCHES, FK_MAX_BRANCHES + 1,
                                FK_META_MAX - 1, FK_META_MAX, FK_META_MAX + 1, FK_MAX_PAGES - 1, FK_MAX_PAGES, FK_MAX_PAGES + 1, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu };
  uint32_t k = rng_below(r, (uint32_t)(sizeof v / sizeof v[0]) + 3);
  if (k < sizeof v / sizeof v[0]) return v[k];
  return k == sizeof v / sizeof v[0] ? (uint32_t)rng_next(r) : rng_below(r, 40);
}
static size_t pick_off(const Img *im, Rng *r) {
  if (!im->n) return 0;
  uint32_t k = rng_below(r, 10);
  if (k < 3 && im->n > 2048) return rng_below(r, 2048);
  if (k < 6) {
    Rec rc[MAXREC];
    int n = parse_recs(im, rc, MAXREC);
    if (n) { const Rec *x = &rc[rng_below(r, (uint32_t)n)]; size_t o = x->off + rng_below(r, RECH + 16); return o < im->n ? o : im->n - 1; }
  }
  return rng_below(r, (uint32_t)im->n);
}

static void mut_bytes(Img *im, Rng *r) {
  if (!im->n) return;
  uint32_t n = 1 + rng_below(r, 8);
  for (uint32_t i = 0; i < n; i++) {
    size_t o = pick_off(im, r);
    switch (rng_below(r, 4)) {
      case 0: im->p[o] ^= (uint8_t)(1u << rng_below(r, 8)); break;
      case 1: im->p[o] = 0; break;
      case 2: im->p[o] = 0xff; break;
      default: im->p[o] = (uint8_t)rng_next(r); break;
    }
  }
}
static void mut_truncate(Img *im, Rng *r) {
  Rec rc[MAXREC];
  int n = parse_recs(im, rc, MAXREC);
  size_t at;
  if (n && rng_below(r, 3) != 0) {
    const Rec *x = &rc[rng_below(r, (uint32_t)n)];
    uint32_t k = rng_below(r, 4);
    at = k == 0 ? x->off : k == 1 ? x->off + RECH : k == 2 ? x->off + RECH + x->len : x->off + rng_below(r, RECH + x->len + 1);
  } else {
    at = rng_below(r, (uint32_t)im->n + 1);
  }
  if (at < im->n) im->n = at;
}
static void mut_extend(Img *im, Rng *r) {
  size_t add = 1 + rng_below(r, 9000), old = im->n;
  img_resize(im, old + add);
  uint32_t k = rng_below(r, 3);
  for (size_t i = 0; i < add; i++) im->p[old + i] = k == 0 ? 0 : k == 1 ? (uint8_t)rng_next(r) : (uint8_t)(i * 31);
}
static void mut_zero_range(Img *im, Rng *r) {
  if (!im->n) return;
  size_t o = pick_off(im, r), len = 1 + rng_below(r, 6000);
  if (o + len > im->n) len = im->n - o;
  memset(im->p + o, rng_below(r, 4) ? 0 : 0xff, len);
}
static void mut_splice(Img *im, Rng *r) {
  if (im->n < 64) return;
  size_t len = 1 + rng_below(r, 5000), a = rng_below(r, (uint32_t)im->n), b = rng_below(r, (uint32_t)im->n);
  if (a + len > im->n) len = im->n - a;
  if (b + len > im->n) len = im->n - b;
  memmove(im->p + b, im->p + a, len);
}
static void mut_header(Img *im, Rng *r) {
  if (im->n < HDR) return;
  uint8_t *h = im->p;
  static const uint32_t ver[] = { 0, 1, 2, 3, 4, 5, 6, 0xffffffffu };
  switch (rng_below(r, 4)) {
    case 0: wr32(h + 8, ver[rng_below(r, 8)]); break;
    case 1: wr32(h + 12, rng_below(r, 3) == 0 ? interesting(r) : rng_below(r, 2) ? 512 : 8192); break;
    case 2: memcpy(h, rng_below(r, 2) ? "FORKDBS?" : "SQLite f", 8); break;
    default: h[rng_below(r, 24)] ^= (uint8_t)(1u << rng_below(r, 8)); break;
  }
  if (rng_below(r, 5) != 0) wr32(h + 16, fk_crc32c(0, h, 16)); /* usually a header that passes its own checksum */
}
static void mut_marker(Img *im, Rng *r) {
  if (im->n < 1056) return;
  uint8_t *m = im->p + 512 + 512 * rng_below(r, 2);
  uint64_t end[] = { 0, HDR, HDR - 1, im->n, im->n + 1, im->n + 4096, ~0ull, rng_next(r) };
  memcpy(m, "FKMK", 4);
  wr64(m + 8, rng_below(r, 3) == 0 ? rng_next(r) : 1 + rng_below(r, 1000));
  wr64(m + 16, end[rng_below(r, 8)]);
  if (rng_below(r, 20) != 0) wr32(m + 4, fk_crc32c(0, m + 8, 16));
}
static void mut_garbage(Img *im, Rng *r) {
  uint32_t k = rng_below(r, 5);
  size_t n = k == 0 ? rng_below(r, 64) : rng_below(r, 20000);
  img_resize(im, n);
  for (size_t i = 0; i < n; i++) im->p[i] = k == 1 ? 0 : (uint8_t)rng_next(r);
  if (k >= 3 && n >= HDR) { memcpy(im->p, "FORKDBST", 8); wr32(im->p + 8, 4); wr32(im->p + 12, FK_PAGE); wr32(im->p + 16, fk_crc32c(0, im->p, 16)); }
  if (k == 4 && n >= HDR + RECH) { wr32(im->p + HDR, 0x31524b46u); wr16(im->p + HDR + 4, T_PAGE); wr64(im->p + HDR + 8, 1); wr32(im->p + HDR + 16, 1); wr32(im->p + HDR + 20, 0); }
}
static void insert_gap(Img *im, size_t at, size_t n) { img_resize(im, im->n + n); memmove(im->p + at + n, im->p + at, im->n - n - at); }
static void remove_range(Img *im, size_t at, size_t n) { memmove(im->p + at, im->p + at + n, im->n - at - n); im->n -= n; }

/* A record rewritten with a valid checksum, and usually a repaired chain behind it, so that scan() gets past the CRC to the checks that follow. */
static void mut_record(Img *im, Rng *r) {
  Rec rc[MAXREC];
  int n = parse_recs(im, rc, MAXREC);
  if (!n) { mut_bytes(im, r); return; }
  int i = (int)rng_below(r, (uint32_t)n);
  if (rng_below(r, 2)) for (int t = 0; t < 8 && (rc[i].type == T_PAGE || rc[i].type == T_CPAGE); t++) i = (int)rng_below(r, (uint32_t)n); /* favour the rare types */
  size_t o = rc[i].off;
  uint32_t len = rc[i].len;
  int repair = rng_below(r, 10) < 7;
  switch (rng_below(r, 9)) {
    case 0: wr16(im->p + o + 4, (uint16_t)(rng_below(r, 4) ? rng_below(r, 13) : rng_next(r))); break;
    case 1: { /* a different payload length, with the rest of the file moved: a structurally valid record of the wrong size */
      static const uint32_t cand[] = { 0, 1, 4, 8, 12, 16, 20, 24, PAGEP - 1, PAGEP, PAGEP + 1, 600 };
      uint32_t nl = rng_below(r, 3) ? cand[rng_below(r, 12)] : len + (rng_below(r, 2) ? 1 : (uint32_t)-1);
      if (nl > PAGEP * 2) nl = PAGEP;
      if (nl > len) { insert_gap(im, o + RECH + len, nl - len); for (uint32_t k = len; k < nl; k++) im->p[o + RECH + k] = (uint8_t)rng_below(r, 3); }
      else if (nl < len) remove_range(im, o + RECH + nl, len - nl);
      wr32(im->p + o + 16, nl);
      break;
    }
    case 2: case 3: case 4: { /* a field of the payload set to an extreme value */
      if (len < 4) break;
      uint32_t k = rng_below(r, 4) ? rng_below(r, len < 40 ? len / 4 : 10) : rng_below(r, len / 4);
      wr32(im->p + o + RECH + 4 * k, interesting(r));
      break;
    }
    case 5: wr64(im->p + o + 8, rng_below(r, 2) ? rd64(im->p + o + 8) + 1 : rng_next(r)); break;
    case 6: wr32(im->p + o + 20, (uint32_t)rng_next(r)); break;
    case 7: { /* duplicate the record */
      insert_gap(im, o + RECH + len, RECH + len);
      memmove(im->p + o + RECH + len, im->p + o, RECH + len);
      break;
    }
    default: { /* delete it */
      if (o + RECH + len <= im->n) remove_range(im, o, RECH + len);
      break;
    }
  }
  if (repair) repair_chain(im, i < n ? i : n - 1); else fix_crc(im, o);
  if (rng_below(r, 10) < 5) wipe_markers(im);
}

/* The snapshot stream knows its own layout (version 4): u32 branches, u32 layers; per layer u32 parent, u32 size, u32 n, then n x (u32 page, u64 offset); per branch u8 alive,
 * u32 head layer; u32 last; u64 state counter; per branch u64 state id, u32 metadata length, the metadata. Mutating it blindly rarely reaches the checks behind the first few,
 * so this one parses it and breaks exactly one rule that a valid stream keeps. benign: change a byte of some branch's metadata instead (nothing but the stream checksum can notice).
 * Returns 0 if the stream does not have the expected shape or has nothing to break. */
static int snap_smart(uint8_t *st, size_t n, Rng *r, int benign) {
  if (n < 16) return 0;
  uint32_t nb = rd32(st), nl = rd32(st + 4);
  if (nb == 0 || nb > FK_MAX_BRANCHES || nl == 0 || nl > n / 12) return 0;
  size_t pos = 8, lay[256], cnt[256];
  if (nl > 256) return 0;
  for (uint32_t i = 0; i < nl; i++) {
    if (pos + 12 > n) return 0;
    uint32_t c = rd32(st + pos + 8);
    lay[i] = pos; cnt[i] = c;
    if ((uint64_t)c * 12 > n - pos - 12) return 0;
    pos += 12 + (size_t)c * 12;
  }
  size_t alive_at[FK_MAX_BRANCHES];
  uint32_t alive_n = 0;
  size_t br = pos;
  if (pos + 5 * (size_t)nb + 4 + 8 > n) return 0;
  for (uint32_t b = 0; b < nb; b++) if (st[br + 5 * b]) alive_at[alive_n++] = br + 5 * b;
  pos = br + 5 * (size_t)nb + 4;
  size_t ctr_at = pos, stat[FK_MAX_BRANCHES], mla[FK_MAX_BRANCHES];
  uint32_t ml[FK_MAX_BRANCHES];
  pos += 8;
  for (uint32_t b = 0; b < nb; b++) {
    if (pos + 12 > n) return 0;
    stat[b] = pos; mla[b] = pos + 8; ml[b] = rd32(st + pos + 8);
    if (ml[b] > n - pos - 12) return 0;
    pos += 12 + ml[b];
  }
  if (!alive_n) return 0;
  uint32_t last_alive = 0;
  for (uint32_t b = 0; b < nb; b++) if (st[br + 5 * b]) last_alive = b;
  if (benign) {
    uint32_t with[FK_MAX_BRANCHES], k = 0;
    for (uint32_t b = 0; b < nb; b++) if (ml[b]) with[k++] = b;
    if (!k) return 0;
    uint32_t b = with[rng_below(r, k)];
    st[mla[b] + 4 + rng_below(r, ml[b])] ^= (uint8_t)(1u << rng_below(r, 8));
    return 1;
  }
  switch (rng_below(r, 4)) {
    case 0: wr32(st + alive_at[rng_below(r, alive_n)] + 1, rng_below(r, 3) == 0 ? ~0u : nl + rng_below(r, 4)); return 1;        /* a head layer that does not exist */
    case 1: { uint32_t b = 0; do b = rng_below(r, nb); while (!st[br + 5 * b]); wr64(st + stat[b], rd64(st + ctr_at) + 1 + rng_below(r, 3)); return 1; } /* a state id above the counter */
    case 2: { size_t after = mla[last_alive] + 4, rem = n - after; if (rem + 1 > FK_META_MAX) return 0; wr32(st + mla[last_alive], (uint32_t)(rem + 1)); return 1; } /* metadata longer than what is left */
    default: {
      uint32_t i = rng_below(r, nl), tries = 0;
      while (cnt[i] < 2 && tries++ < nl) i = (i + 1) % nl;
      if (cnt[i] < 2) return 0;
      uint32_t j = rng_below(r, (uint32_t)cnt[i] - 1);
      uint8_t t[12], *a = st + lay[i] + 12 + 12 * (size_t)j, *c = a + 12;
      memcpy(t, a, 12); memcpy(a, c, 12); memcpy(c, t, 12);                                                                        /* two page entries out of order */
      return 1;
    }
  }
}

/* The snapshot stream (the layer graph of a compacted file): mutate it, split it back into the same records, recompute SNAPEND, repair the chain. */
static int mut_snapshot(Img *im, Rng *r, int stale) {
  Rec rc[MAXREC];
  int n = parse_recs(im, rc, MAXREC), first = -1, last = -1, end = -1;
  for (int i = 0; i < n; i++) {
    if (rc[i].type == T_SNAP) { if (first < 0) first = i; last = i; }
    else if (rc[i].type == T_SNAPEND && last >= 0 && end < 0) end = i;
  }
  if (first < 0 || end < 0) return 0;
  size_t slen = 0;
  for (int i = first; i <= last; i++) if (rc[i].type == T_SNAP) slen += rc[i].len;
  uint8_t *stream = malloc(slen ? slen : 1);
  size_t at = 0;
  for (int i = first; i <= last; i++) if (rc[i].type == T_SNAP) { memcpy(stream + at, im->p + rc[i].off + RECH, rc[i].len); at += rc[i].len; }
  uint8_t *orig = malloc(slen ? slen : 1);
  memcpy(orig, stream, slen);
  uint32_t k = 1 + rng_below(r, 3);
  for (uint32_t m = 0; m < k && slen >= 8 && stale <= 1; m++) {
    size_t pos = rng_below(r, 3) ? rng_below(r, (uint32_t)(slen < 96 ? slen : 96)) : rng_below(r, (uint32_t)slen);
    switch (rng_below(r, 4)) {
      case 0: case 1: if (pos + 4 <= slen) wr32(stream + pos, interesting(r)); break;
      case 2: if (pos + 8 <= slen) wr64(stream + pos, rng_below(r, 2) ? rng_next(r) : (uint64_t)interesting(r)); break;
      default: stream[pos] ^= (uint8_t)(1u << rng_below(r, 8)); break;
    }
  }
  if (stale >= 3 && !snap_smart(stream, slen, r, stale == 3)) { free(orig); free(stream); return 0; }
  if (stale == 1 && slen >= 8 && !memcmp(orig, stream, slen)) stream[0] ^= 1; /* the stale case must really change the stream */
  free(orig);
  at = 0;
  for (int i = first; i <= last; i++) if (rc[i].type == T_SNAP) { memcpy(im->p + rc[i].off + RECH, stream + at, rc[i].len); at += rc[i].len; }
  uint8_t *e = im->p + rc[end].off + RECH;
  if (stale == 2 && rc[end].len >= 12) wr64(e, slen + 1 + rng_below(r, 1000)); /* only the length is wrong; the checksum of the stream still matches */
  if ((!stale || stale == 4) && rc[end].len >= 12) { wr64(e, slen); wr32(e + 8, fk_crc32c(0, stream, slen)); } /* stale: the stream checksum and length are left as they were */
  free(stream);
  repair_chain(im, first);
  if (!stale && rng_below(r, 10) < 6) wipe_markers(im);
  return 1;
}

/* Append a verbatim copy of 1-4 earlier records (valid checksums, wrong sequence number and chain): recovery must treat it as a torn tail and cut it off. */
static void mut_tail(Img *im, Rng *r) {
  Rec rc[MAXREC];
  int n = parse_recs(im, rc, MAXREC);
  if (n < 2) return;
  int i = (int)rng_below(r, (uint32_t)n), cnt = 1 + (int)rng_below(r, 4);
  if (i + cnt > n) cnt = n - i;
  size_t start = rc[i].off, end = rc[i + cnt - 1].off + RECH + rc[i + cnt - 1].len, old = im->n;
  img_resize(im, old + (end - start));
  memcpy(im->p + old, im->p + start, end - start);
}

/* A record that is well formed (valid checksum, the next sequence number, the chain intact) but means something that cannot be: recovery must refuse to apply it and cut the log there. */
static void append_rec(Img *im, uint16_t type, const uint8_t *pay, uint32_t len) {
  Rec rc[MAXREC];
  int n = parse_recs(im, rc, MAXREC);
  if (!n) return;
  size_t last = rc[n - 1].off, old = im->n;
  img_resize(im, old + RECH + len);
  uint8_t *h = im->p + old;
  memset(h, 0, RECH);
  wr32(h, 0x31524b46u); wr16(h + 4, type);
  wr64(h + 8, rd64(im->p + last + 8) + 1);
  wr32(h + 16, len);
  wr32(h + 20, rd32(im->p + last + 24));
  memcpy(h + RECH, pay, len);
  fix_crc(im, old);
}
static void mut_craft(Img *im, Rng *r) {
  static uint8_t p[PAGEP + 64];
  memset(p, 0, sizeof p);
  uint32_t len = 0, ml = FK_META_MAX + 1 + rng_below(r, 200);
  uint16_t type = 0;
  switch (rng_below(r, 8)) {
    case 0: type = T_COMMIT; len = 20; wr32(p + 4, 1 + rng_below(r, 5)); wr32(p + 8, 1 + rng_below(r, 20)); break;           /* pages announced, none seen */
    case 1: type = T_COMMIT; len = 20; wr32(p + 8, FK_MAX_PAGES + 1 + rng_below(r, 1000)); break;                            /* a size over the limit */
    case 2: type = T_FORK; len = 8; wr32(p + 4, FK_MAX_BRANCHES + rng_below(r, 3)); break;                                    /* a branch id over the limit (== nb on the full table) */
    case 3: type = T_META; len = 8 + ml; wr32(p + 4, ml); memset(p + 8, 'm', ml); break;                                      /* metadata over the limit */
    case 4: type = T_FORKM; len = 12 + ml; wr32(p + 4, 1); wr32(p + 8, ml); memset(p + 12, 'm', ml); break;
    case 5: type = T_DISCARD; len = 4; break;                                                                                 /* the root */
    case 6: type = T_DISCARD; len = 4; wr32(p, 1000 + rng_below(r, 1000)); break;                                            /* a branch that does not exist */
    default: type = T_PAGE; len = PAGEP - 1; break;                                                                           /* the wrong length */
  }
  append_rec(im, type, p, len);
}

enum { M_BYTES, M_TRUNC, M_EXTEND, M_ZERO, M_SPLICE, M_HEADER, M_MARKER, M_GARBAGE, M_RECORD, M_SNAPSHOT, M_TAIL, M_STALE, M_CRAFT, NMUT };
static const char *mut_name[NMUT] = { "bytes", "truncate", "extend", "zero-range", "splice", "header", "marker", "garbage", "record", "snapshot", "tail-replay", "stale-snap", "crafted-tail" };
static int apply_mut(Img *im, Rng *r, int has_snap) {
  uint32_t k = rng_below(r, 100);
  int m = k < 14 ? M_BYTES : k < 22 ? M_TRUNC : k < 26 ? M_EXTEND : k < 31 ? M_ZERO : k < 35 ? M_SPLICE : k < 41 ? M_HEADER : k < 46 ? M_MARKER : k < 50 ? M_GARBAGE : k < 84 ? M_RECORD : M_SNAPSHOT;
  if (m == M_SNAPSHOT && !has_snap) m = M_RECORD;
  switch (m) {
    case M_BYTES: mut_bytes(im, r); break;
    case M_TRUNC: mut_truncate(im, r); break;
    case M_EXTEND: mut_extend(im, r); break;
    case M_ZERO: mut_zero_range(im, r); break;
    case M_SPLICE: mut_splice(im, r); break;
    case M_HEADER: mut_header(im, r); break;
    case M_MARKER: mut_marker(im, r); break;
    case M_GARBAGE: mut_garbage(im, r); break;
    case M_RECORD: mut_record(im, r); break;
    default: if (!mut_snapshot(im, r, 0)) { mut_record(im, r); } break;
  }
  return m;
}

/* ---------------------------------------------------------------- one input */
typedef struct {
  uint64_t inputs, opened, rej_corrupt, rej_other, reinit_nonempty, reopen_checked, read_errors, op_errors, compactions, violations;
  uint64_t by_mut[NMUT], opened_by_mut[NMUT];
} Stats;
static Stats st;
static uint64_t g_seed, g_iter;
static int g_mut, g_si;
static const Img *g_cur;
static int g_verbose;
static void free_seeds(void) { for (int i = 0; i < NSEEDS; i++) img_free(&seeds[i]); }

static void dump_current(const char *path) {
  if (!g_cur || !g_cur->p) return;
  FILE *f = fopen(path, "wb");
  if (f) { fwrite(g_cur->p, 1, g_cur->n, f); fclose(f); }
}
static void announce(const char *what) {
  fprintf(stderr, "\nfuzz %s: seed=%" PRIu64 " input=%" PRIu64 " (seed image %d, first mutator %s); image written to /tmp/fuzz-fail.bin\n  reproduce: fuzz_open --one %" PRIu64 " %" PRIu64 " file\n",
          what, g_seed, g_iter, g_si, mut_name[g_mut], g_seed, g_iter);
  dump_current("/tmp/fuzz-fail.bin");
}
static void on_alarm(int sig) { (void)sig; announce("HANG (no result in 30 s)"); _exit(3); }
#if defined(__SANITIZE_ADDRESS__)
static void on_death(void) { announce("FAILURE (sanitizer or abort)"); }
#endif

typedef struct { int alive, clean; uint64_t dig, state; } BDig;
static BDig g_ref[NSEEDS][FK_MAX_BRANCHES]; /* what each seed image reads as */
static uint32_t g_refnb[NSEEDS];
static int g_expect; /* 0: nothing known; 1: the store must read exactly as its seed (or be refused); 2: it must be refused */
static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }

#define VIOL(...) do { st.violations++; fprintf(stderr, "VIOLATION (seed %" PRIu64 " input %" PRIu64 "): ", g_seed, g_iter); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

/* Look at everything a caller can look at; returns the number of branches. */
static uint32_t walk(fk_store *s, BDig *bd, uint64_t *read_errs) {
  uint32_t nb = fk_branch_count(s);
  int v = fk_version(s);
  if (v < 1 || v > 4) VIOL("version %d", v);
  if (nb == 0 || nb > FK_MAX_BRANCHES) { VIOL("branch count %u", nb); return 0; }
  if (!fk_branch_alive(s, 0)) VIOL("branch 0 is not alive");
  uint8_t pg[FK_PAGE];
  for (uint32_t b = 0; b < nb; b++) {
    bd[b].alive = fk_branch_alive(s, b); bd[b].clean = 1; bd[b].dig = 1469598103934665603ull;
    if (!bd[b].alive) continue;
    uint32_t pages = 0, depth = 0;
    if (fk_size(s, b, &pages) != FK_OK) { VIOL("fk_size failed on alive branch %u", b); continue; }
    if (pages > FK_MAX_PAGES) { VIOL("branch %u has %u pages (limit %u)", b, pages, FK_MAX_PAGES); continue; }
    (void)fk_branch_depth(s, b, &depth);
    uint64_t state = 0; (void)fk_state(s, b, &state); bd[b].state = state;
    uint8_t meta[FK_META_MAX]; size_t ml = 0;
    if (fk_meta_get(s, b, meta, sizeof meta, &ml) == FK_OK) {
      if (ml > FK_META_MAX) VIOL("metadata length %zu", ml);
      else { bd[b].dig = fnv(bd[b].dig, meta, ml); bd[b].dig = fnv(bd[b].dig, (const uint8_t *)&ml, sizeof ml); }
    }
    uint32_t step = pages <= 256 ? 1 : pages / 192;
    for (uint32_t p = 0, k = 0; p < pages && k < 256; p += step, k++) {
      int rc = fk_read(s, b, p, pg);
      if (rc != FK_OK) { VIOL("page %u of branch %u of an opened store cannot be read (%d)", p, b, rc); bd[b].clean = 0; (*read_errs)++; break; }
      bd[b].dig = fnv(bd[b].dig, pg, FK_PAGE);
    }
    bd[b].dig = fnv(bd[b].dig, (const uint8_t *)&pages, 4);
  }
  (void)fk_layer_count(s); (void)fk_index_bytes(s);
  return nb;
}

static void compute_refs(void) {
  for (int i = 0; i < NSEEDS; i++) {
    SimDisk d; sim_init(&d, seeds[i].p, seeds[i].n);
    fk_io io = sim_io(&d); fk_store *s = NULL;
    if (fk_open(&io, &s) != FK_OK) { fprintf(stderr, "fuzz: a seed image does not open\n"); exit(2); }
    uint64_t ig = 0;
    g_refnb[i] = walk(s, g_ref[i], &ig);
    fk_close(s); sim_free(&d);
  }
}

static int header_valid(const Img *im) {
  if (im->n < HDR || memcmp(im->p, "FORKDBST", 8) != 0) return 0;
  uint32_t v = rd32(im->p + 8);
  return v >= 1 && v <= 4 && rd32(im->p + 12) == FK_PAGE && rd32(im->p + 16) == fk_crc32c(0, im->p, 16);
}
static int all_zero(const Img *im) { for (size_t i = 0; i < im->n; i++) if (im->p[i]) return 0; return 1; }

static void run_input(const Img *im, Rng *r, int mut) {
  g_cur = im;
  st.inputs++;
  if (mut >= 0) st.by_mut[mut]++;
  alarm(30);
  SimDisk d; sim_init(&d, im->p, im->n);
  fk_io io = sim_io(&d);
  fk_store *s = NULL;
  int rc = fk_open(&io, &s);
  if (rc != FK_OK) {
    if (s) VIOL("fk_open failed (%d) but returned a store", rc);
    if (rc > 0) VIOL("fk_open returned %d", rc);
    if (g_expect == 1) VIOL("an image that is a valid store followed by records that must be cut off was refused (%d)", rc);
    if (rc == FK_ECORRUPT) st.rej_corrupt++; else st.rej_other++;
    sim_free(&d);
    alarm(0);
    return;
  }
  st.opened++;
  if (mut >= 0) st.opened_by_mut[mut]++;
  if (g_expect == 2) VIOL("a snapshot that is stale (checksum or length no longer matches) or breaks a rule of its own layout was opened");
  int bad_header = im->n > 0 && !header_valid(im);
  if (bad_header && !all_zero(im)) {
    if (im->n > HDR) VIOL("a file of %zu bytes with an invalid header was opened", im->n);
    else { st.reinit_nonempty++; if (getenv("FUZZ_STRICT")) VIOL("a non-empty file with an invalid header (%zu bytes) was re-initialised", im->n); }
  }
  BDig before[FK_MAX_BRANCHES];
  memset(before, 0, sizeof before);
  uint32_t nb = walk(s, before, &st.read_errors);
  if (g_expect == 1) {
    const BDig *ref = g_ref[g_si];
    if (nb != g_refnb[g_si]) VIOL("records after the end of the log changed the number of branches (%u, was %u)", nb, g_refnb[g_si]);
    else for (uint32_t b = 0; b < nb; b++)
      if (before[b].alive != ref[b].alive || (before[b].alive && (before[b].dig != ref[b].dig || before[b].state != ref[b].state))) { VIOL("records after the end of the log changed branch %u", b); break; }
  }
  /* write to it: a new branch with a few pages, metadata on branch 0, sometimes a discard, sometimes a compaction */
  int clean = 1;
  uint32_t dst = 0;
#define OP(call) do { int e_ = (call); if (e_ != FK_OK && e_ != FK_EVERSION && e_ != FK_EINVAL && e_ != FK_ENOENT) { st.op_errors++; clean = 0; } } while (0)
  if (fk_fork(s, 0, &dst) == FK_OK) {
    uint8_t pg[FK_PAGE]; uint32_t sz = 0;
    if (fk_size(s, dst, &sz) == FK_OK) {
      /* grow the new branch past its size: the pages that appear were never written on it, so they must read as zeros (a page hidden by an earlier shrink must stay hidden) */
      uint32_t base = sz < 3 ? 3 : sz, grow = base + 8 <= FK_MAX_PAGES ? 8 : 0;
      OP(fk_txn_begin(s, dst));
      for (uint32_t i = 0; i < 3; i++) { page_fill(pg, 900 + i); OP(fk_txn_write(s, i, pg)); }
      int crc_ = fk_txn_commit(s, base + grow);
      OP(crc_);
      if (crc_ == FK_OK) {
        uint64_t ns = 0;
        if (fk_state(s, dst, &ns) == FK_OK) for (uint32_t b = 0; b < nb; b++) if (b != dst && before[b].alive && before[b].state == ns) VIOL("the state id %" PRIu64 " given to a commit is already held by branch %u", ns, b);
      }
      if (crc_ == FK_OK) for (uint32_t i = 0; i < grow; i++) {
        if (fk_read(s, dst, base + i, pg) != FK_OK) { clean = 0; break; }
        for (uint32_t k = 0; k < FK_PAGE; k++) if (pg[k]) { VIOL("page %u appeared by growing branch %u and is not zero (byte %u = %u)", base + i, dst, k, pg[k]); break; }
      }
    }
    if (rng_below(r, 10) < 3) OP(fk_discard(s, dst));
  }
  if (dst && rng_below(r, 2)) OP(fk_meta_set(s, dst, "fuzz", 4)); /* on the new fork: the branches that were there are compared, metadata included, after the reopen */
  if (rng_below(r, 4) == 0) { int did = 0; OP(fk_compact_if_needed(s, 0, &did)); st.compactions += (uint64_t)did; }
  if (fk_is_failed(s)) clean = 0;
  fk_close(s);
  /* what a restart finds */
  uint64_t vn = 0;
  const uint8_t *v = sim_visible(&d, &vn);
  SimDisk d2; sim_init(&d2, v, vn);
  sim_free(&d);
  fk_io io2 = sim_io(&d2);
  fk_store *s2 = NULL;
  int rc2 = fk_open(&io2, &s2);
  if (rc2 != FK_OK) {
    if (clean) VIOL("a store that opened, was written to cleanly and closed does not reopen (%d)", rc2);
  } else {
    st.reopen_checked++;
    BDig after[FK_MAX_BRANCHES];
    memset(after, 0, sizeof after);
    uint64_t ignore = 0;
    uint32_t nb2 = walk(s2, after, &ignore);
    if (clean) for (uint32_t b = 0; b < nb && b < nb2; b++) {
      if (!before[b].alive || !before[b].clean || b == dst) continue;
      if (!after[b].alive) VIOL("branch %u was alive and is gone after a clean close and reopen", b);
      else if (after[b].clean && after[b].dig != before[b].dig) VIOL("branch %u changed content across a clean close and reopen", b);
      else if (!after[b].clean) VIOL("branch %u was readable and is not after a reopen", b);
    }
    fk_close(s2);
  }
  sim_free(&d2);
  alarm(0);
}

static void gen_input(uint64_t seed, uint64_t iter, Img *out, Rng *r, int *first_mut) {
  uint64_t x = seed ^ (iter * 0x9e3779b97f4a7c15ull);
  rng_seed(r, splitmix(&x));
  g_si = (int)rng_below(r, NSEEDS);
  img_set(out, seeds[g_si].p, seeds[g_si].n);
  g_expect = 0;
  { uint32_t k = rng_below(r, 100);
    if (k < 8) { mut_tail(out, r); g_expect = 1; *first_mut = M_TAIL; return; }
    if (k < 12 && seed_has_snap[g_si] && mut_snapshot(out, r, 1)) { g_expect = 2; *first_mut = M_STALE; return; }
    if (k < 15 && seed_has_snap[g_si] && mut_snapshot(out, r, 2)) { g_expect = 2; *first_mut = M_STALE; return; }
    if (k < 19 && seed_has_snap[g_si] && mut_snapshot(out, r, 3)) { g_expect = 2; *first_mut = M_STALE; return; }
    if (k < 25 && seed_has_snap[g_si] && mut_snapshot(out, r, 4)) { g_expect = 2; *first_mut = M_STALE; return; }
    if (k < 33) { mut_craft(out, r); g_expect = 1; *first_mut = M_CRAFT; return; } }
  uint32_t nm = 1 + (rng_below(r, 10) >= 7) + (rng_below(r, 10) >= 9);
  for (uint32_t i = 0; i < nm; i++) { int m = apply_mut(out, r, seed_has_snap[g_si]); if (i == 0) *first_mut = m; }
}

int main(int argc, char **argv) {
  uint64_t inputs = 20000, seed = 1;
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_alarm; sigaction(SIGALRM, &sa, NULL);
#if defined(__SANITIZE_ADDRESS__)
  __sanitizer_set_death_callback(on_death);
#endif
  build_seeds();
  compute_refs();
  if (argc >= 4 && !strcmp(argv[1], "--one")) {
    seed = strtoull(argv[2], NULL, 10);
    uint64_t n = strtoull(argv[3], NULL, 10);
    Img im = {0}; Rng r; int m = 0;
    g_seed = seed; g_iter = n; g_verbose = 1;
    gen_input(seed, n, &im, &r, &m); g_mut = m;
    if (argc >= 5) { FILE *f = fopen(argv[4], "wb"); if (f) { fwrite(im.p, 1, im.n, f); fclose(f); printf("wrote %zu bytes to %s\n", im.n, argv[4]); } }
    run_input(&im, &r, m);
    printf("input %" PRIu64 " of seed %" PRIu64 ": opened=%" PRIu64 " rejected=%" PRIu64 " violations=%" PRIu64 "\n", n, seed, st.opened, st.rej_corrupt + st.rej_other, st.violations);
    g_cur = NULL; img_free(&im); free_seeds();
    return st.violations ? 1 : 0;
  }
  if (argc >= 2) inputs = strtoull(argv[1], NULL, 10);
  if (argc >= 3) seed = strtoull(argv[2], NULL, 10);
  g_seed = seed;
  Img im = {0};
  long peak_kb = 0;
  for (uint64_t i = 0; i < inputs; i++) {
    Rng r; int m = 0;
    g_iter = i;
    gen_input(seed, i, &im, &r, &m);
    g_mut = m;
    run_input(&im, &r, m);
    { struct rusage u; getrusage(RUSAGE_SELF, &u); if (u.ru_maxrss > peak_kb + 20000) { peak_kb = u.ru_maxrss; fprintf(stderr, "peak memory grew to %ld MB at input %" PRIu64 " (first mutator %s)\n", peak_kb / 1024, i, mut_name[m]); } }
    if (st.violations > 20) { fprintf(stderr, "fuzz: stopping after 20 violations\n"); break; }
  }
  g_cur = NULL; img_free(&im); free_seeds();
  struct rusage ru; getrusage(RUSAGE_SELF, &ru);
  printf("first mutator   inputs  opened\n");
  for (int m = 0; m < NMUT; m++) printf("  %-12s %7" PRIu64 " %7" PRIu64 "\n", mut_name[m], st.by_mut[m], st.opened_by_mut[m]);
  printf("fuzz: seed=%" PRIu64 " inputs=%" PRIu64 " opened=%" PRIu64 " rejected_corrupt=%" PRIu64 " rejected_other=%" PRIu64 " reinitialised_nonempty=%" PRIu64
         " reopen_checked=%" PRIu64 " read_errors=%" PRIu64 " op_errors=%" PRIu64 " compactions=%" PRIu64 " peak_rss_mb=%ld violations=%" PRIu64 "\n",
         seed, st.inputs, st.opened, st.rej_corrupt, st.rej_other, st.reinit_nonempty, st.reopen_checked, st.read_errors, st.op_errors, st.compactions, ru.ru_maxrss / 1024, st.violations);
  printf("%s\n", st.violations ? "FUZZ FAILED" : "TOTAL FUZZ FAILURES: 0");
  return st.violations ? 1 : 0;
}
