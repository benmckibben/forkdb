/* crash_test: power-loss testing of fkstore on simdisk.
 *
 * For seeded random workloads (commits, forks, discards over up to 8 branches), the power is cut before every I/O event,
 * the surviving disk image is built in every pessimistic way simdisk allows, and the recovered store must equal the
 * oracle state after exactly the acknowledged operations, or after those plus the one in flight. Also checks: open always
 * succeeds, recovery is idempotent, a crash DURING recovery is survivable, the store is appendable after recovery,
 * and a failed fsync makes the store fail-stop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simdisk.h"

#define MAXBR 8
#define MAXPG 6
#define MAXOPS 16

typedef struct { int alive; uint32_t size; uint8_t has[MAXPG]; uint8_t pg[MAXPG][FK_PAGE]; } MB;
typedef struct { uint32_t nb; MB b[MAXBR]; } Model;
typedef struct { int kind; /* 0 commit, 1 fork, 2 discard */ uint32_t br, np, pgno[3], new_size, expect_dst; uint64_t key; } Op;

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
}
static void model_apply(Model *M, const Op *o) {
  if (o->kind == 0) model_commit(M, o);
  else if (o->kind == 1) { M->b[M->nb] = M->b[o->br]; M->nb++; }
  else M->b[o->br].alive = 0;
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
    if (roll < 25 && M->nb < MAXBR) { o.kind = 1; o.br = alive[rng_below(&r, na)]; o.expect_dst = M->nb; }
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
      rc = fk_txn_write(s, o->pgno[j], buf);
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
  }
  return 1;
}

typedef struct { long images, ok_old, ok_new, violations, open_fail, nested, cont; } Stats;
typedef struct {
  const Model *states;
  int cand[2], ncand;
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

static void verify_image(void *vc, const uint8_t *img, uint64_t len);

static void nested(const Ctx *c, const uint8_t *img, uint64_t len) {
  /* Recovery itself does I/O (it truncates the torn tail). Cut the power at every one of its events. */
  SimDisk probe;
  sim_init(&probe, img, len);
  fk_io io = sim_io(&probe);
  fk_store *s = NULL;
  if (fk_open(&io, &s) != 0) { sim_free(&probe); return; }
  long events = probe.events;
  fk_close(s);
  sim_free(&probe);
  for (long m = 0; m < events; m++) {
    SimDisk d;
    sim_init(&d, img, len);
    d.crash_at = m;
    io = sim_io(&d);
    s = NULL;
    if (fk_open(&io, &s) == 0) fk_close(s);
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
  fk_io io = sim_io(&d);
  fk_store *s = NULL;
  int rc = fk_open(&io, &s);
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
    sim_init(&d2, d.dur, d.dur_len);
    io = sim_io(&d2);
    fk_store *s2 = NULL;
    if (fk_open(&io, &s2) != 0 || !equal_state(s2, &expect)) {
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
  if (mode == 0) d.crash_at = N; else d.fsync_fail_at = N;
  fk_io io = sim_io(&d);
  fk_store *s = NULL;
  int acked = 0, inflight = 0;
  int rc = fk_open(&io, &s);
  if (rc == 0) {
    for (int i = 0; i < K; i++) {
      rc = run_op(s, &ops[i]);
      if (rc) { inflight = 1; break; }
      acked++;
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
    fk_close(s);
  }
  if (events_out) *events_out = d.events;
  if (rc != 0 && mode == 0 && !d.dead) {
    printf("  VIOLATION seed=%llu unexpected error rc=%d without a power cut\n", (unsigned long long)seed, rc);
    st->violations++;
  }
  Ctx c;
  memset(&c, 0, sizeof c);
  c.states = states; c.st = st; c.seed = seed; c.N = N; c.tag = mode ? "after-failed-fsync" : "power-cut";
  c.rseed = seed * 31 + (uint64_t)N * 17;
  c.ops = ops; c.K = K;
  c.cand[0] = acked; c.ncand = 1;
  if (inflight && acked < K) c.cand[c.ncand++] = acked + 1;
  Rng r;
  rng_seed(&r, c.rseed);
  sim_images(&d, &r, verify_image, &c);
  sim_free(&d);
  free(states);
  return 0;
}

int main(int argc, char **argv) {
  int seeds = argc > 1 ? atoi(argv[1]) : 10, K = argc > 2 ? atoi(argv[2]) : 7;
  if (K > MAXOPS) K = MAXOPS;
  Stats st;
  memset(&st, 0, sizeof st);
  long cases = 0, total_events = 0;
#ifdef FK_BUG_SKIP_FSYNC
  printf("MUTANT: commits are acknowledged without fsync\n");
#endif
#ifdef FK_BUG_NO_PREVCHAIN
  printf("MUTANT: recovery does not check the record chain\n");
#endif
#ifdef FK_BUG_NO_TRUNC
  printf("MUTANT: recovery does not truncate the torn tail\n");
#endif
#ifdef FK_BUG_EARLY_MARKER
  printf("MUTANT: the durable-end marker is written before the data it covers is synced\n");
#endif
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
  printf("\nworkloads x cut points: %ld (incl. fsync-failure cases), I/O events cut: %ld\n", cases, total_events);
  printf("post-crash images checked: %ld (recovered to acknowledged state: %ld, to acknowledged+in-flight: %ld)\n", st.images, st.ok_old, st.ok_new);
  printf("append-after-recovery checks: %ld, crash-during-recovery cut points: %ld\n", st.cont, st.nested);
  printf("TOTAL VIOLATIONS=%ld\n", st.violations);
  return st.violations ? 1 : 0;
}
