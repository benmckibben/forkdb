/* crash_test: power-loss testing of fkstore on simdisk.
 *
 * For seeded random workloads (commits, forks, discards and compactions over up to 8 branches), the power is cut before every I/O event,
 * the surviving disk image is built in every pessimistic way simdisk allows, and the recovered store must equal the
 * oracle state after exactly the acknowledged operations, or after those plus the one in flight. Also checks: open always
 * succeeds, recovery is idempotent, a crash DURING recovery is survivable, the store is appendable after recovery,
 * and a failed fsync makes the store fail-stop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simdisk.h"

#ifndef FK_MAX_BRANCHES
#error "build with -DFK_MAX_BRANCHES=8: the store and this model must agree on the number of branch ids"
#endif
#define MAXBR FK_MAX_BRANCHES /* 8: small, so that ids are reused all the time */
#define MAXPG 20 /* enough distinct pages for a layer's index table to grow several times */
#define MAXOPS 16

#define MAXMETA 40
/* cls: which content the branch holds; two live branches have equal state ids in the store exactly when their cls are equal */
typedef struct { int alive; uint32_t size; uint8_t has[MAXPG]; uint8_t pg[MAXPG][FK_PAGE]; uint32_t cls; uint8_t nmeta; uint8_t meta[MAXMETA]; } MB;
typedef struct { uint32_t nb, last, ctr; MB b[MAXBR]; } Model; /* nb: one more than the highest id used; last: id of the latest fork */
/* kinds: 0 commit, 1 fork, 2 discard, 3 compact (changes nothing the oracle can see), 4 reset (br takes the content of pgno[0]), 5 set the
 * metadata of br (np bytes derived from key), 6 fork with metadata (np bytes from key) */
typedef struct { int kind; uint32_t br, np, pgno[3], new_size, expect_dst; uint64_t key; } Op;
static void meta_bytes(uint64_t key, uint32_t n, uint8_t *out) { for (uint32_t i = 0; i < n; i++) out[i] = (uint8_t)(key * 31 + i * 7 + 1); }

static void gen_page(uint64_t key, uint8_t *buf) {
  Rng r;
  rng_seed(&r, key);
  for (size_t i = 0; i < FK_PAGE; i += 8) { uint64_t v = rng_next(&r); memcpy(buf + i, &v, 8); }
}
static void model_commit(Model *M, const Op *o) {
  MB *m = &M->b[o->br];
  for (uint32_t pg = o->new_size; pg < m->size && pg < MAXPG; pg++) m->has[pg] = 0;
  for (uint32_t j = 0; j < o->np; j++) { gen_page(o->key + j, m->pg[o->pgno[j]]); m->has[o->pgno[j]] = 1; }
  m->size = o->new_size;
  m->cls = ++M->ctr;
}
static void model_apply(Model *M, const Op *o) {
  if (o->kind == 0) model_commit(M, o);
  else if (o->kind == 1 || o->kind == 6) {
    M->b[o->expect_dst] = M->b[o->br]; /* same content, same cls */
    M->b[o->expect_dst].nmeta = 0;
    if (o->kind == 6) { M->b[o->expect_dst].nmeta = (uint8_t)o->np; meta_bytes(o->key, o->np, M->b[o->expect_dst].meta); }
    M->last = o->expect_dst;
    if (o->expect_dst >= M->nb) M->nb = o->expect_dst + 1;
  }
  else if (o->kind == 2) { M->b[o->br].alive = 0; M->b[o->br].nmeta = 0; }
  else if (o->kind == 4) { MB keep = M->b[o->br]; M->b[o->br] = M->b[o->pgno[0]]; M->b[o->br].nmeta = keep.nmeta; memcpy(M->b[o->br].meta, keep.meta, MAXMETA); }
  else if (o->kind == 5) { M->b[o->br].nmeta = (uint8_t)o->np; meta_bytes(o->key, o->np, M->b[o->br].meta); }
  /* kind 3: compaction preserves every branch, page and size */
}
/* The id the next fork must get, written out independently of the store: ids 1..MAXBR-1 form a ring; start just after the id of
 * the latest fork; take the first one that is not alive. */
static int model_next_id(const Model *M, uint32_t *id) {
  int ring = (int)MAXBR - 1;
  for (int k = 1; k <= ring; k++) {
    int pos = (((int)M->last - 1 + k) % ring + ring) % ring; /* position of the candidate in the ring; last = 0 counts as position -1 */
    uint32_t cand = (uint32_t)pos + 1;
    if (cand >= M->nb || !M->b[cand].alive) { *id = cand; return 1; }
  }
  return 0;
}
static int gen_ops(uint64_t seed, int K, Op *ops, Model *states) {
  Rng r;
  rng_seed(&r, seed * 7919 + 13);
  memset(&states[0], 0, sizeof states[0]);
  states[0].nb = 1;
  states[0].b[0].alive = 1;
  for (int i = 0; i < K; i++) {
    Model *M = &states[i];
    uint32_t alive[MAXBR], na = 0;
    for (uint32_t b = 0; b < M->nb; b++) if (M->b[b].alive) alive[na++] = b;
    Op o;
    memset(&o, 0, sizeof o);
    uint32_t roll = rng_below(&r, 100);
    uint32_t next_id = 0;
    if (roll < 25 && model_next_id(M, &next_id)) { o.kind = 1; o.br = alive[rng_below(&r, na)]; o.expect_dst = next_id; }
    else if (roll >= 88) o.kind = 3;
    else if (roll >= 40 && roll < 48 && na >= 2) { /* reset: dst takes the content of another live branch */
      uint32_t d = rng_below(&r, na), sidx = rng_below(&r, na - 1);
      if (sidx >= d) sidx++;
      o.kind = 4; o.br = alive[d]; o.pgno[0] = alive[sidx];
    }
    else if (roll >= 48 && roll < 54) { o.kind = 5; o.br = alive[rng_below(&r, na)]; o.np = rng_below(&r, MAXMETA + 1); o.key = seed * 1000003ull + (uint64_t)i * 16 + 2; }
    else if (roll >= 54 && roll < 60 && model_next_id(M, &next_id)) { o.kind = 6; o.br = alive[rng_below(&r, na)]; o.expect_dst = next_id; o.np = rng_below(&r, MAXMETA + 1); o.key = seed * 1000003ull + (uint64_t)i * 16 + 3; }
    else if (roll < 40) {
      uint32_t cand[MAXBR], nc = 0;
      for (uint32_t k = 0; k < na; k++) if (alive[k] != 0) cand[nc++] = alive[k];
      if (nc) { o.kind = 2; o.br = cand[rng_below(&r, nc)]; }
      else o.kind = 0;
    }
    if (o.kind == 0) {
      o.br = alive[rng_below(&r, na)];
      o.new_size = 1 + rng_below(&r, MAXPG);
      o.np = 1 + rng_below(&r, 3);
      for (uint32_t j = 0; j < o.np; j++) o.pgno[j] = rng_below(&r, o.new_size);
      o.key = seed * 1000003ull + (uint64_t)i * 16 + 1;
    }
    ops[i] = o;
    states[i + 1] = states[i];
    model_apply(&states[i + 1], &o);
  }
  return K;
}

static int run_op(fk_store *s, const Op *o) {
  uint8_t buf[FK_PAGE];
  if (o->kind == 0) {
    int rc = fk_txn_begin(s, o->br);
    if (rc) return rc;
    for (uint32_t j = 0; j < o->np; j++) {
      gen_page(o->key + j, buf);
      if (j % 2) { /* half the pages go in through the ownership-transfer call, so the power-loss workloads cover it */
        uint8_t *d = malloc(FK_PAGE);
        if (!d) { fk_txn_abort(s); return FK_ENOMEM; }
        memcpy(d, buf, FK_PAGE);
        rc = fk_txn_write_owned(s, o->pgno[j], d);
        if (rc) free(d);
      } else {
        rc = fk_txn_write(s, o->pgno[j], buf);
      }
      if (rc) { fk_txn_abort(s); return rc; }
    }
    rc = fk_txn_commit(s, o->new_size);
    if (rc) fk_txn_abort(s);
    return rc;
  }
  if (o->kind == 1) {
    uint32_t dst = 0;
    int rc = fk_fork(s, o->br, &dst);
    if (rc == 0 && dst != o->expect_dst) return -99;
    return rc;
  }
  if (o->kind == 3) return fk_compact(s);
  if (o->kind == 4) return fk_reset(s, o->br, o->pgno[0]);
  if (o->kind == 5) { uint8_t mb[MAXMETA]; meta_bytes(o->key, o->np, mb); return fk_meta_set(s, o->br, mb, o->np); }
  if (o->kind == 6) {
    uint8_t mb[MAXMETA];
    uint32_t dst = 0;
    meta_bytes(o->key, o->np, mb);
    int rc = fk_fork_meta(s, o->br, mb, o->np, &dst);
    if (rc == 0 && dst != o->expect_dst) return -99;
    return rc;
  }
  return fk_discard(s, o->br);
}

static char why[256];
static int equal_state(fk_store *s, const Model *M) {
  if (fk_branch_count(s) != M->nb) { snprintf(why, sizeof why, "branch count %u, expected %u", fk_branch_count(s), M->nb); return 0; }
  static uint8_t zero[FK_PAGE];
  uint8_t buf[FK_PAGE];
  for (uint32_t b = 0; b < MAXBR; b++) {
    int alive = fk_branch_alive(s, b);
    if (alive != M->b[b].alive) { snprintf(why, sizeof why, "branch %u alive=%d, expected %d", b, alive, M->b[b].alive); return 0; }
    if (!alive) continue;
    uint32_t sz = 0;
    fk_size(s, b, &sz);
    if (sz != M->b[b].size) { snprintf(why, sizeof why, "branch %u size %u, expected %u", b, sz, M->b[b].size); return 0; }
    for (uint32_t pg = 0; pg < sz; pg++) {
      int rc = fk_read(s, b, pg, buf);
      if (rc) { snprintf(why, sizeof why, "branch %u page %u read rc=%d", b, pg, rc); return 0; }
      if (memcmp(buf, M->b[b].has[pg] ? M->b[b].pg[pg] : zero, FK_PAGE)) { snprintf(why, sizeof why, "branch %u page %u content differs", b, pg); return 0; }
    }
    if (fk_read(s, b, sz, buf) != FK_ENOENT) { snprintf(why, sizeof why, "branch %u: page at size is readable", b); return 0; }
    uint8_t mb[MAXMETA + 8];
    size_t ml = 99;
    if (fk_meta_get(s, b, mb, sizeof mb, &ml) != 0 || ml != M->b[b].nmeta || memcmp(mb, M->b[b].meta, ml)) { snprintf(why, sizeof why, "branch %u: metadata is %zu bytes, expected %u (or its bytes differ)", b, ml, M->b[b].nmeta); return 0; }
  }
  /* state ids: equal exactly when the model says the content is the same (a commit makes a new one, fork and reset copy) */
  for (uint32_t i = 0; i < MAXBR; i++) {
    if (!M->b[i].alive) continue;
    for (uint32_t j = i + 1; j < MAXBR; j++) {
      if (!M->b[j].alive) continue;
      uint64_t si = 0, sj = 0;
      if (fk_state(s, i, &si) != 0 || fk_state(s, j, &sj) != 0) { snprintf(why, sizeof why, "fk_state failed for %u or %u", i, j); return 0; }
      if ((si == sj) != (M->b[i].cls == M->b[j].cls)) { snprintf(why, sizeof why, "state ids of branches %u and %u are %s, the model says the content is %s", i, j, si == sj ? "equal" : "different", M->b[i].cls == M->b[j].cls ? "the same" : "not the same"); return 0; }
    }
  }
  /* The layer tree is as small as it can be: every layer has a live branch on it or at least two children (a leaf needs a branch,
   * and a layer without a branch that has one child would have been folded into it), so with n live branches there are at most
   * n layers with a branch and at most n - 1 with two or more children. */
  uint32_t nalive = 0;
  for (uint32_t b = 0; b < MAXBR; b++) nalive += (uint32_t)fk_branch_alive(s, b);
  if (fk_layer_count(s) > 2 * nalive - 1) { snprintf(why, sizeof why, "%u layers for %u live branches (at most %u are needed)", fk_layer_count(s), nalive, 2 * nalive - 1); return 0; }
  return 1;
}

typedef struct { long images, ok_old, ok_new, violations, open_fail, nested, cont, compactions; long refused, nosync_ok, lagged, lost_all, pd_checks; } Stats;
typedef struct {
  const Model *states;
  int cand[MAXOPS + 2], ncand;
  int nosync, acked; /* nosync: the store ran with no syncs, so a recovered state may be any acknowledged one, and refusing to open is allowed */
  Stats *st;
  uint64_t seed;
  long N;
  const char *tag;
  uint64_t rseed;
  int depth;
  const Op *ops;
  int K;
} Ctx;

static void report(const Ctx *c, const char *what) {
  if (c->st->violations <= 6)
    printf("  VIOLATION seed=%llu cut-before-event=%ld %s: %s (%s)\n", (unsigned long long)c->seed, c->N, c->tag, what, why);
}
static int match(const Ctx *c, fk_store *s) {
  for (int i = 0; i < c->ncand; i++) if (equal_state(s, &c->states[c->cand[i]])) return i;
  return -1;
}

/* CRASH_PREALLOC=n runs every store through the preallocation layer with n-byte chunks (store/io_prealloc.c), so that the cut points
 * include the zero fills and the cuts they leave behind. A wrapped io is released when its variable goes out of scope. */
static uint64_t g_prealloc;
static fk_io mkio(SimDisk *d) {
  fk_io io = sim_io(d);
  if (g_prealloc) fk_prealloc_wrap(&io, g_prealloc);
  return io;
}
static void rmio(fk_io *io) { fk_prealloc_release(io); }
#define CLEANUP_IO __attribute__((cleanup(rmio)))

static void verify_image(void *vc, const uint8_t *img, uint64_t len);

static void nested(const Ctx *c, const uint8_t *img, uint64_t len) {
  /* Recovery itself does I/O (it truncates the torn tail). Cut the power at every one of its events. */
  SimDisk probe;
  sim_init(&probe, img, len);
  fk_io io CLEANUP_IO = mkio(&probe);
  fk_store *s = NULL;
  if (fk_open(&io, &s) != 0) { sim_free(&probe); return; }
  long events = probe.events;
  fk_close(s);
  sim_free(&probe);
  for (long m = 0; m < events; m++) {
    SimDisk d;
    sim_init(&d, img, len);
    d.crash_at = m;
    fk_io io2 CLEANUP_IO = mkio(&d);
    s = NULL;
    if (fk_open(&io2, &s) == 0) fk_close(s);
    Ctx c2 = *c;
    c2.depth = 1;
    c2.tag = "crash-during-recovery";
    Rng r;
    rng_seed(&r, c->rseed + (uint64_t)m + 99);
    sim_images(&d, &r, verify_image, &c2);
    sim_free(&d);
    c->st->nested++;
  }
}

static void verify_image(void *vc, const uint8_t *img, uint64_t len) {
  Ctx *c = vc;
  c->st->images++;
  SimDisk d;
  sim_init(&d, img, len);
  fk_io io CLEANUP_IO = mkio(&d);
  fk_store *s = NULL;
  int rc = fk_open(&io, &s);
  if (rc != 0 && c->nosync) { /* a refusal is an allowed answer after a power cut with no syncs: the file says it cannot be trusted */
    c->st->refused++;
    sim_free(&d);
    return;
  }
  if (rc != 0) {
    c->st->violations++;
    c->st->open_fail++;
    snprintf(why, sizeof why, "fk_open rc=%d", rc);
    report(c, "recovery failed to open the store");
    sim_free(&d);
    return;
  }
  int m = match(c, s);
  if (m < 0) {
    c->st->violations++;
    report(c, "recovered state is neither the acknowledged state nor acknowledged+in-flight");
    fk_close(s);
    sim_free(&d);
    return;
  }
  if (m == 0) c->st->ok_old++; else c->st->ok_new++;
  if (c->nosync && c->depth == 0) {
    c->st->nosync_ok++;
    if (c->cand[m] < c->acked) c->st->lagged++;
    if (c->cand[m] == 0 && c->acked > 0) c->st->lost_all++;
  }
  if (c->depth == 0) {
    /* The store must be usable after recovery: replay the rest of the workload on it (so new records of many sizes land
     * on top of whatever torn tail was there), reopen, and compare with the oracle's final state. When nothing is left
     * to replay, do one extra commit instead. */
    int ci = c->cand[m], rc2 = 0;
    Model expect;
    if (ci < c->K) {
      expect = c->states[c->K];
      for (int i = ci; i < c->K && rc2 == 0; i++) rc2 = run_op(s, &c->ops[i]);
    } else {
      expect = c->states[ci];
      Op o;
      memset(&o, 0, sizeof o);
      o.kind = 0; o.br = 0; o.np = 1; o.pgno[0] = 0; o.new_size = expect.b[0].size > 1 ? expect.b[0].size : 1;
      o.key = 424242 + c->seed;
      rc2 = run_op(s, &o);
      model_apply(&expect, &o);
    }
    fk_close(s);
    if (rc2 != 0) {
      c->st->violations++;
      snprintf(why, sizeof why, "operation after recovery rc=%d", rc2);
      report(c, "store not usable after recovery");
      sim_free(&d);
      return;
    }
    SimDisk d2;
    uint64_t dl = 0;
    const uint8_t *dimg = sim_durable(&d, &dl);
    sim_init(&d2, dimg, dl);
    fk_io io2 CLEANUP_IO = mkio(&d2);
    fk_store *s2 = NULL;
    if (fk_open(&io2, &s2) != 0 || !equal_state(s2, &expect)) {
      c->st->violations++;
      if (!s2) snprintf(why, sizeof why, "reopen failed");
      report(c, "state after recovery + one more commit + reopen is wrong");
    }
    if (s2) fk_close(s2);
    sim_free(&d2);
    c->st->cont++;
    nested(c, img, len);
  } else {
    fk_close(s);
  }
  sim_free(&d);
}

/* One workload with the power cut before event N (N < 0: run to the end). mode 1: instead make fsync number N fail. */
static long run_case(uint64_t seed, int K, long N, int mode, Stats *st, long *events_out) {
  Op ops[MAXOPS];
  Model *states = malloc((size_t)(MAXOPS + 1) * sizeof *states);
  gen_ops(seed, K, ops, states);
  SimDisk d;
  sim_init(&d, NULL, 0);
  if (mode == 0) d.crash_at = N;
  else if (mode == 2) { d.crash_at = N; d.nosync = 1; }
  else d.fsync_fail_at = N;
  fk_io io CLEANUP_IO = mkio(&d);
  fk_store *s = NULL;
  int acked = 0, inflight = 0;
  long ev_work = 0; /* events of the workload itself, not of the checks after it */
  int rc = fk_open(&io, &s);
  if (rc == 0) {
    for (int i = 0; i < K; i++) {
      rc = run_op(s, &ops[i]);
      ev_work = d.events;
      if (rc && ops[i].kind == 3 && mode == 1 && !fk_is_failed(s)) { rc = 0; acked++; continue; } /* a compaction that fails before the swap leaves the store untouched and usable */
      if (rc) { inflight = 1; break; }
      acked++;
      if (ops[i].kind == 3) {
        /* the in-memory state after a compaction must equal the oracle, and the file must now hold nothing but live data */
        uint64_t fb = 0, lb = 0, fb2 = 0, lb2 = 0;
        if (N < 0 && mode == 0) st->compactions++;
        if (!equal_state(s, &states[i + 1])) {
          printf("  VIOLATION seed=%llu compaction changed the visible state (%s)\n", (unsigned long long)seed, why);
          st->violations++;
        } else if (fk_stats(s, &fb, &lb) != 0 || fb != lb) {
          printf("  VIOLATION seed=%llu after compaction the file is %llu bytes but the live data is %llu\n", (unsigned long long)seed, (unsigned long long)fb, (unsigned long long)lb);
          st->violations++;
        } else if (N < 0 && mode == 0 && (fk_compact(s) != 0 || fk_stats(s, &fb2, &lb2) != 0 || fb2 != fb || !equal_state(s, &states[i + 1]))) {
          printf("  VIOLATION seed=%llu a second compaction changed the file (%llu -> %llu bytes) or the state\n", (unsigned long long)seed, (unsigned long long)fb, (unsigned long long)fb2);
          st->violations++;
        }
      }
    }
    if (rc && mode == 1) {
      /* fail-stop: after an I/O error nothing may be accepted, reads included */
      uint8_t buf[FK_PAGE];
      int r2 = run_op(s, &ops[acked < K ? acked : K - 1]);
      int r3 = fk_read(s, 0, 0, buf);
      if (r2 != FK_EFAILED || (r3 != FK_EFAILED && r3 != FK_ENOENT)) {
        printf("  VIOLATION seed=%llu fsync#%ld failed but the store kept going (next op rc=%d, read rc=%d)\n", (unsigned long long)seed, N, r2, r3);
        st->violations++;
      }
    }
    if (mode == 2 && N < 0 && rc == 0 && acked == K) {
      /* the process dies (no close), the machine stays up: the operating system still has every write, so nothing may be lost */
      uint64_t vl = 0;
      const uint8_t *vv = sim_visible(&d, &vl);
      SimDisk dv;
      sim_init(&dv, vv, vl);
      fk_io iov CLEANUP_IO = mkio(&dv);
      fk_store *sv = NULL;
      st->pd_checks++;
      if (fk_open(&iov, &sv) != 0 || !equal_state(sv, &states[K])) {
        printf("  VIOLATION seed=%llu the process died with no sync and the machine stayed up, and the data was lost (%s)\n", (unsigned long long)seed, sv ? why : "open failed");
        st->violations++;
      }
      if (sv) fk_close(sv);
      sim_free(&dv);
    }
    fk_close(s);
  }
  if (events_out) *events_out = ev_work;
  if (rc != 0 && mode == 0 && !d.dead) {
    printf("  VIOLATION seed=%llu unexpected error rc=%d without a power cut\n", (unsigned long long)seed, rc);
    st->violations++;
  }
  Ctx c;
  memset(&c, 0, sizeof c);
  c.states = states; c.st = st; c.seed = seed; c.N = N; c.tag = mode == 2 ? "power-cut-no-syncs" : mode ? "after-failed-fsync" : "power-cut";
  c.rseed = seed * 31 + (uint64_t)N * 17;
  c.ops = ops; c.K = K;
  c.cand[0] = acked; c.ncand = 1;
  if (inflight && acked < K) c.cand[c.ncand++] = acked + 1;
  Rng r;
  rng_seed(&r, c.rseed);
  if (mode == 2) {
    /* with no syncs any prefix of the acknowledged history may be what the disk kept; the open must fail or return one of them */
    c.nosync = 1; c.acked = acked;
    for (int j = acked - 1; j >= 0; j--) c.cand[c.ncand++] = j;
    sim_images_prefix(&d, &r, 24, verify_image, &c);
  }
  sim_images(&d, &r, verify_image, &c);
  sim_free(&d);
  free(states);
  return 0;
}

/* No power cut: long workloads with several compactions in them, checked against the oracle after every operation and again
 * after a close and reopen. Cheap, so it can run thousands of seeds; it is what exercises layer folding. */
static long logic_case(uint64_t seed, int K, long *compactions) {
  Op ops[MAXOPS];
  Model *states = malloc((size_t)(MAXOPS + 1) * sizeof *states);
  gen_ops(seed * 977 + 5, K, ops, states);
  SimDisk d;
  sim_init(&d, NULL, 0);
  fk_io io CLEANUP_IO = mkio(&d);
  fk_store *s = NULL;
  long bad = 0;
  if (fk_open(&io, &s) != 0) { free(states); sim_free(&d); return 1; }
  for (int i = 0; i < K && !bad; i++) {
    int rc = run_op(s, &ops[i]);
    if (rc) { printf("  VIOLATION logic seed=%llu op %d (kind %d) failed rc=%d\n", (unsigned long long)seed, i, ops[i].kind, rc); bad = 1; break; }
    if (!equal_state(s, &states[i + 1])) { printf("  VIOLATION logic seed=%llu after op %d (kind %d): %s\n", (unsigned long long)seed, i, ops[i].kind, why); bad = 1; break; }
    if (ops[i].kind == 3) {
      uint64_t fb = 0, lb = 0;
      (*compactions)++;
      if (fk_stats(s, &fb, &lb) != 0 || fb != lb) { printf("  VIOLATION logic seed=%llu after compaction the file is %llu bytes, live data %llu\n", (unsigned long long)seed, (unsigned long long)fb, (unsigned long long)lb); bad = 1; }
    }
  }
  fk_close(s);
  if (!bad) {
    uint64_t dl = 0;
    const uint8_t *dimg = sim_durable(&d, &dl);
    SimDisk d2;
    sim_init(&d2, dimg, dl);
    fk_io io2 CLEANUP_IO = mkio(&d2);
    fk_store *s2 = NULL;
    if (fk_open(&io2, &s2) != 0 || !equal_state(s2, &states[K])) { printf("  VIOLATION logic seed=%llu reopen: %s\n", (unsigned long long)seed, s2 ? why : "open failed"); bad = 1; }
    if (s2) fk_close(s2);
    sim_free(&d2);
  }
  sim_free(&d);
  free(states);
  return bad;
}

int main(int argc, char **argv) {
  int seeds = argc > 1 ? atoi(argv[1]) : 10, K = argc > 2 ? atoi(argv[2]) : 7;
  if (K > MAXOPS) K = MAXOPS;
  Stats st;
  memset(&st, 0, sizeof st);
  long cases = 0, total_events = 0;
  int logic_seeds = argc > 3 ? atoi(argv[3]) : 3000;
  if (getenv("CRASH_PREALLOC")) g_prealloc = strtoull(getenv("CRASH_PREALLOC"), NULL, 10);
  if (g_prealloc) printf("every store runs through the preallocation layer, %llu-byte chunks\n", (unsigned long long)g_prealloc);
#ifdef FK_BUG_SKIP_FSYNC
  printf("MUTANT: commits are acknowledged without fsync\n");
#endif
#ifdef FK_BUG_NO_PREVCHAIN
  printf("MUTANT: recovery does not check the record chain\n");
#endif
#ifdef FK_BUG_NO_TRUNC
  printf("MUTANT: recovery does not truncate the torn tail\n");
#endif
#ifdef FK_BUG_COMPACT_NO_TMP_SYNC
  printf("MUTANT: compaction swaps in the new file without syncing it\n");
#endif
#ifdef FK_BUG_COMPACT_NO_DIRSYNC
  printf("MUTANT: compaction does not sync the directory after the rename\n");
#endif
#ifdef FK_BUG_COMPACT_DROP_TOMB
  printf("MUTANT: compaction drops tombstones that hide a lower layer\n");
#endif
#ifdef FK_BUG_ID_REUSE_ALIVE
  printf("MUTANT: fork can hand out the id of a live branch\n");
#endif
#ifdef FK_BUG_LAYER_NO_FREE
  printf("MUTANT: a discarded branch's layers are never freed\n");
#endif
#ifdef FK_BUG_LAYER_NO_FOLD
  printf("MUTANT: dead layers are freed but single-child layers are never folded\n");
#endif
#ifdef FK_BUG_ID_REPLAY_STRICT
  printf("MUTANT: recovery rejects a fork that reuses an id\n");
#endif
#ifdef FK_BUG_ID_NO_CURSOR
  printf("MUTANT: the allocation cursor is not restored from a snapshot\n");
#endif
#ifdef FK_BUG_EARLY_MARKER
  printf("MUTANT: the durable-end marker is written before the data it covers is synced\n");
#endif
  {
    long lc = 0;
    for (int sd = 1; sd <= logic_seeds; sd++) st.violations += logic_case((uint64_t)sd, MAXOPS, &lc);
    printf("logic pass: %d workloads of %d operations with %ld compactions, no power cut: violations so far %ld\n", logic_seeds, MAXOPS, lc, st.violations);
  }
  for (int sd = 1; sd <= seeds; sd++) {
    long events = 0;
    run_case((uint64_t)sd, K, -1, 0, &st, &events); /* no crash: also measures the number of events */
    cases++;
    for (long N = 0; N < events; N++) { run_case((uint64_t)sd, K, N, 0, &st, NULL); cases++; }
    for (long N = 0; N < events; N++) { run_case((uint64_t)sd, K, N, 1, &st, NULL); cases++; }
    total_events += events;
    printf("seed %d: %ld I/O events; images so far %ld, violations %ld\n", sd, events, st.images, st.violations);
    fflush(stdout);
  }
  {
    /* The same workloads with a store opened without syncs (FK_FILE_NOSYNC): nothing is ever durable until the power cut. The claim is weaker than the one above and is
     * tested as exactly that: the open fails, or returns one of the states the workload acknowledged (usually an older one), and the store is usable afterwards; and a
     * process that dies with the machine up loses nothing. It is never allowed to open as something that was not a state of the workload. */
    Stats ns;
    memset(&ns, 0, sizeof ns);
    for (int sd = 1; sd <= seeds; sd++) {
      long events = 0;
      run_case((uint64_t)sd, K, -1, 2, &ns, &events);
      long stride = events / 12 > 0 ? events / 12 : 1;
      for (long N = 0; N < events; N += stride) run_case((uint64_t)sd, K, N, 2, &ns, NULL);
    }
    printf("\nno-sync pass (store opened with FK_FILE_NOSYNC): images %ld; refused to open %ld; opened to an acknowledged state %ld (older than the last acknowledged: %ld, empty: %ld); process-death checks %ld; violations %ld\n",
           ns.images, ns.refused, ns.nosync_ok, ns.lagged, ns.lost_all, ns.pd_checks, ns.violations);
    st.violations += ns.violations;
  }
  printf("\nworkloads x cut points: %ld (incl. fsync-failure cases), I/O events cut: %ld\n", cases, total_events);
  printf("post-crash images checked: %ld (recovered to acknowledged state: %ld, to acknowledged+in-flight: %ld)\n", st.images, st.ok_old, st.ok_new);
  printf("append-after-recovery checks: %ld, crash-during-recovery cut points: %ld, compactions in the workloads: %ld\n", st.cont, st.nested, st.compactions);
  printf("TOTAL VIOLATIONS=%ld\n", st.violations);
  return st.violations ? 1 : 0;
}
