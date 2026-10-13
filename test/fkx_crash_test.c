/* Power-loss test of fkx. The same simulated disk as the store's test (test/simdisk.c): random workloads of fork, snapshot, discard, reset,
 * promote, undo, writes and compaction run on it, the power is cut before every I/O event, and every post-crash image (any subset of the
 * pending writes, torn) is opened with fkx. After every cut:
 *   - fkx opens, and its index is consistent (fkx_check: unique names and uids, every fork has its own base, every undo entry's snapshots exist,
 *     no internal branch its owner does not list);
 *   - what the user can see (the named branches: which exist, their kind, their content, how many undo points they list) is exactly one of the
 *     states the operation in flight could have left: before, after, or (for promote and undo, which change a branch and then its undo list in two
 *     records) the one in between;
 *   - fkx_gc finds nothing more to collect, a new fork and its discard work, and a reopen of what that left shows the same;
 *   - an undo cut after its reset finishes (the entry is removed) and gives the "after" state.
 * Some images are also recovered with a second power cut in the middle of the recovery (the garbage collection writes). Build: -DFK_MAX_BRANCHES=64. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fkx/fkx_internal.h"
#include "simdisk.h"

#define MAXN 8
#define KEEP 2
#define MAXOPS 24

typedef struct { int alive, kind, val, nundo, org_slot; uint64_t gen, org_gen; uint32_t cls, base_cls; struct { int bval; uint32_t bcls, acls; } undo[KEEP]; int base_val; } MB;
typedef struct { MB b[MAXN]; uint32_t ctr; uint64_t gens; } Model;
typedef struct { int kind, br, src, to, v; } Op; /* 0 put, 1 fork, 2 snapshot, 3 discard, 4 reset, 5 promote, 6 undo, 7 compact */

static const char *nm(int i) { static char b[MAXN][8]; if (i == 0) return "main"; snprintf(b[i], sizeof b[i], "n%d", i); return b[i]; }

static void m_init(Model *M) { memset(M, 0, sizeof *M); M->b[0].alive = 1; M->b[0].kind = FKX_ROOT; M->b[0].val = -1; M->b[0].cls = ++M->ctr; M->b[0].gen = ++M->gens; }
static int writable(const MB *b) { return b->alive && (b->kind == FKX_ROOT || b->kind == FKX_FORK); }

/* mid: the view after only the first visible change of a promote or an undo (the target's content, before its undo list changes) */
static int m_apply(Model *M, const Op *o, Model *mid) {
  int has_mid = 0;
  MB *t = &M->b[o->br];
  switch (o->kind) {
    case 0: t->val = o->v; t->cls = ++M->ctr; break;
    case 1: case 2: {
      MB *s = &M->b[o->src];
      MB n;
      memset(&n, 0, sizeof n);
      n.alive = 1; n.kind = o->kind == 1 ? FKX_FORK : FKX_SNAPSHOT; n.val = s->val; n.cls = s->cls; n.base_cls = s->cls; n.base_val = s->val; n.gen = ++M->gens; n.org_slot = o->src; n.org_gen = s->gen;
      *t = n;
      break;
    }
    case 3: memset(t, 0, sizeof *t); break;
    case 4: if (o->to < 0) { t->val = t->base_val; t->cls = t->base_cls; } else { t->val = M->b[o->to].val; t->cls = M->b[o->to].cls; } break;
    case 5: {
      MB *f = t, *p = &M->b[f->org_slot];
      *mid = *M; mid->b[f->org_slot].val = f->val; mid->b[f->org_slot].cls = f->cls; has_mid = 1;
      for (int e = KEEP - 1; e > 0; e--) p->undo[e] = p->undo[e - 1];
      p->undo[0].bval = p->val; p->undo[0].bcls = p->cls; p->undo[0].acls = f->cls;
      if (p->nundo < KEEP) p->nundo++;
      p->val = f->val; p->cls = f->cls;
      break;
    }
    case 6: {
      *mid = *M; mid->b[o->br].val = t->undo[0].bval; mid->b[o->br].cls = t->undo[0].bcls; has_mid = 1;
      t->val = t->undo[0].bval; t->cls = t->undo[0].bcls;
      for (int e = 1; e < KEEP; e++) t->undo[e - 1] = t->undo[e];
      t->nundo--;
      break;
    }
    default: break;
  }
  return has_mid;
}

static int promote_target(const Model *M, int f) { const MB *b = &M->b[f]; return b->org_slot >= 0 && M->b[b->org_slot].alive && M->b[b->org_slot].gen == b->org_gen && writable(&M->b[b->org_slot]); }

static void gen_ops(uint64_t seed, int K, Op *ops, Model *states, Model *mids, int *has_mid) {
  Rng r;
  rng_seed(&r, seed);
  m_init(&states[0]);
  int vctr = 0;
  for (int i = 0; i < K; i++) {
    Model *M = &states[i];
    Op o;
    int tries = 0;
    for (;;) {
      memset(&o, 0, sizeof o);
      o.to = -1;
      uint32_t roll = rng_below(&r, 100);
      int alive[MAXN], na = 0, wr[MAXN], nw = 0, free_[MAXN], nf = 0;
      for (int s = 0; s < MAXN; s++) { if (M->b[s].alive) alive[na++] = s; else free_[nf++] = s; if (writable(&M->b[s])) wr[nw++] = s; }
      if (roll < 30) { o.kind = 0; o.br = wr[rng_below(&r, (uint32_t)nw)]; o.v = ++vctr; }
      else if (roll < 48 && nf) { o.kind = 1; o.br = free_[rng_below(&r, (uint32_t)nf)]; o.src = alive[rng_below(&r, (uint32_t)na)]; }
      else if (roll < 56 && nf) { o.kind = 2; o.br = free_[rng_below(&r, (uint32_t)nf)]; o.src = alive[rng_below(&r, (uint32_t)na)]; }
      else if (roll < 66 && na > 1) { o.kind = 3; do o.br = alive[rng_below(&r, (uint32_t)na)]; while (o.br == 0); }
      else if (roll < 74) { o.kind = 4; o.br = wr[rng_below(&r, (uint32_t)nw)]; if (M->b[o.br].kind == FKX_FORK && rng_below(&r, 2)) o.to = -1; else { if (na < 2) continue; do o.to = alive[rng_below(&r, (uint32_t)na)]; while (o.to == o.br); } }
      else if (roll < 90) {
        int cand[MAXN], nc = 0;
        for (int s = 0; s < nw; s++) { const MB *f = &M->b[wr[s]]; if (f->kind == FKX_FORK && promote_target(M, wr[s]) && M->b[f->org_slot].cls == f->base_cls && M->b[f->org_slot].cls != f->cls) cand[nc++] = wr[s]; }
        if (!nc) { if (++tries > 40) { o.kind = 7; break; } continue; }
        o.kind = 5; o.br = cand[rng_below(&r, (uint32_t)nc)]; if (0) {}
        break;
      } else if (roll < 96) {
        int cand[MAXN], nc = 0;
        for (int s = 0; s < nw; s++) { const MB *p = &M->b[wr[s]]; if (p->nundo > 0 && p->cls == p->undo[0].acls) cand[nc++] = wr[s]; }
        if (!nc) { if (++tries > 40) { o.kind = 7; break; } continue; }
        o.kind = 6; o.br = cand[rng_below(&r, (uint32_t)nc)];
        break;
      } else o.kind = 7;
      if (o.kind == 0 || o.kind == 1 || o.kind == 2 || o.kind == 3 || o.kind == 4 || o.kind == 7) break;
      if (++tries > 40) { o.kind = 7; break; }
    }
    ops[i] = o;
    states[i + 1] = states[i];
    has_mid[i + 1] = m_apply(&states[i + 1], &o, &mids[i + 1]);
  }
}

static int val(fkx *x, const char *ref) {
  fkx_info in;
  uint8_t pg[FK_PAGE];
  if (fkx_info_get(x, ref, &in) || fk_read(fkx_store_rw(x), in.id, 0, pg)) return -1;
  return pg[0];
}
static char why[300];
static void why_prefix(const char *pre) { char t[300]; snprintf(t, sizeof t, "%s", why); snprintf(why, sizeof why, "%.40s: %.240s", pre, t); }
static int view_equal(fkx *x, const Model *M) {
  int na = 0;
  for (int s = 0; s < MAXN; s++) {
    fkx_info in;
    int rc = fkx_info_get(x, nm(s), &in);
    if (!M->b[s].alive) { if (rc == 0) { snprintf(why, sizeof why, "branch %s exists but should not", nm(s)); return 0; } continue; }
    na++;
    if (rc) { snprintf(why, sizeof why, "branch %s is missing", nm(s)); return 0; }
    if ((int)in.kind != M->b[s].kind) { snprintf(why, sizeof why, "branch %s has kind %d, expected %d", nm(s), (int)in.kind, M->b[s].kind); return 0; }
    int v = val(x, nm(s));
    if (v != M->b[s].val) { snprintf(why, sizeof why, "branch %s holds %d, expected %d", nm(s), v, M->b[s].val); return 0; }
    if (writable(&M->b[s]) && (int)in.n_undo != M->b[s].nundo) { snprintf(why, sizeof why, "branch %s lists %u undo points, expected %d", nm(s), in.n_undo, M->b[s].nundo); return 0; }
  }
  size_t n = 0;
  fkx_list(x, 0, NULL, 0, &n);
  if ((int)n != na) { snprintf(why, sizeof why, "%zu named branches, expected %d", n, na); return 0; }
  return 1;
}

static int run_op(fkx *x, const Op *o) {
  switch (o->kind) {
    case 0: {
      fkx_info in;
      uint8_t pg[FK_PAGE];
      memset(pg, o->v, sizeof pg);
      fk_store *s = fkx_store_rw(x);
      int rc = fkx_info_get(x, nm(o->br), &in);
      if (rc) return rc;
      if ((rc = fk_txn_begin(s, in.id))) return rc;
      if ((rc = fk_txn_write(s, 0, pg))) { fk_txn_abort(s); return rc; }
      return fk_txn_commit(s, 1);
    }
    case 1: return fkx_fork(x, nm(o->src), nm(o->br));
    case 2: return fkx_snapshot(x, nm(o->src), nm(o->br));
    case 3: return fkx_discard(x, nm(o->br));
    case 4: return fkx_reset(x, nm(o->br), o->to < 0 ? NULL : nm(o->to));
    case 5: return fkx_promote(x, nm(o->br), NULL, NULL);
    case 6: return fkx_undo(x, nm(o->br), NULL);
    default: return fk_compact(fkx_store_rw(x));
  }
}

typedef struct { long violations, cases, images, nested; } Stats;
typedef struct {
  const Model *cand[4]; int ncand; int mid_idx; /* index in cand[] of an undo's in-between view, or -1 */
  const Model *after_undo; int undo_op_br;
  Stats *st; uint64_t seed; long N; const char *tag; Rng *rr; long counter; int depth;
} Ctx;

static void report(Ctx *c, const char *what) {
  c->st->violations++;
  printf("  VIOLATION seed=%llu cut=%ld (%s) %s: %s\n", (unsigned long long)c->seed, c->N, c->tag, what, why);
}
static fkx_options mkopt(void) { fkx_options o; fkx_options_default(&o); o.undo_keep = KEEP; o.prealloc_chunk = 0; return o; }

static void leaf(void *vc, const uint8_t *img, uint64_t len);
static void nested_cuts(Ctx *c, const uint8_t *img, uint64_t len) {
  /* recover once with no cut to count the events of the recovery, then cut it at every one of them */
  SimDisk d;
  sim_init(&d, img, len);
  fk_io io = sim_io(&d);
  fkx *x = NULL;
  fkx_options o = mkopt();
  int rc = fkx_open_io(&io, &o, &x);
  long ev = d.events;
  if (x) fkx_close(x);
  sim_free(&d);
  if (rc) return;
  for (long m = 0; m < ev; m++) {
    SimDisk d2;
    sim_init(&d2, img, len);
    d2.crash_at = m;
    fk_io io2 = sim_io(&d2);
    fkx *y = NULL;
    fkx_options o2 = mkopt();
    (void)fkx_open_io(&io2, &o2, &y);
    if (y) fkx_close(y);
    Rng r;
    rng_seed(&r, c->seed * 7 + (uint64_t)m);
    Ctx c2 = *c;
    c2.depth = 1;
    c2.tag = "cut during recovery";
    c2.rr = &r;
    c->st->nested++;
    sim_images(&d2, &r, leaf, &c2);
    sim_free(&d2);
  }
}

static int matches(Ctx *c, fkx *x, int *which) {
  for (int k = 0; k < c->ncand; k++) if (view_equal(x, c->cand[k])) { *which = k; return 1; }
  *which = -1;
  return 0;
}

/* a leaf: open, check, match */
static void verify(Ctx *c, const uint8_t *img, uint64_t len, int deep) {
  c->st->images++;
  SimDisk d;
  sim_init(&d, img, len);
  fk_io io = sim_io(&d);
  fkx *x = NULL;
  fkx_options o = mkopt();
  int rc = fkx_open_io(&io, &o, &x);
  if (rc) { snprintf(why, sizeof why, "fkx_open failed rc=%d: %s", rc, x ? fkx_errmsg(x) : "(no handle)"); report(c, "recovery failed"); sim_free(&d); return; }
  char buf[200];
  int which = -1;
  if (fkx_check(x, buf, sizeof buf)) { snprintf(why, sizeof why, "%s", buf); report(c, "the index is inconsistent after recovery"); }
  else if (!matches(c, x, &which)) {
    char first[200];
    snprintf(first, sizeof first, "%.190s", why);
    snprintf(why, sizeof why, "%s (not one of the %d states the operation could leave)", first, c->ncand);
    report(c, "the visible state is wrong");
  } else if (deep) {
    fkx_gc_report g;
    if (fkx_gc(x, &g) || g.orphan_bases || g.orphan_undo || g.unlisted_undo || g.over_retention) { snprintf(why, sizeof why, "gc after recovery found garbage again (%u %u %u %u)", g.orphan_bases, g.orphan_undo, g.unlisted_undo, g.over_retention); report(c, "recovery left garbage"); }
    if (which == c->mid_idx && c->mid_idx >= 0) {
      /* an undo cut after its reset: running it again finishes it and gives the "after" view */
      if (fkx_undo(x, nm(c->undo_op_br), NULL) != 0 || !view_equal(x, c->after_undo)) { why_prefix("finishing a cut undo"); report(c, "undo did not finish"); }
      which = -2;
    }
    if (fkx_fork(x, "main", "zz-recover") != 0 || fkx_discard(x, "zz-recover") != 0) { snprintf(why, sizeof why, "%s", fkx_errmsg(x)); report(c, "not usable after recovery"); }
    else {
      fkx_close(x);
      x = NULL;
      uint64_t dl = 0;
      const uint8_t *dimg = sim_durable(&d, &dl);
      SimDisk d2;
      sim_init(&d2, dimg, dl);
      fk_io io2 = sim_io(&d2);
      fkx *y = NULL;
      fkx_options o2 = mkopt();
      if (fkx_open_io(&io2, &o2, &y) || fkx_check(y, buf, sizeof buf) || (which >= 0 && !view_equal(y, c->cand[which])) || (which == -2 && !view_equal(y, c->after_undo))) { why_prefix("after one more operation and a reopen"); report(c, "state lost"); }
      if (y) fkx_close(y);
      sim_free(&d2);
    }
  }
  if (x) fkx_close(x);
  sim_free(&d);
}
static void leaf(void *vc, const uint8_t *img, uint64_t len) { verify((Ctx *)vc, img, len, 0); }
static void top(void *vc, const uint8_t *img, uint64_t len) {
  Ctx *c = vc;
  verify(c, img, len, 1);
  if (++c->counter % 6 == 0) nested_cuts(c, img, len);
}

static void run_case(uint64_t seed, int K, long N, Stats *st, long *events_out) {
  Op ops[MAXOPS];
  Model *states = malloc((size_t)(MAXOPS + 1) * sizeof *states), *mids = malloc((size_t)(MAXOPS + 1) * sizeof *mids);
  int has_mid[MAXOPS + 1];
  memset(has_mid, 0, sizeof has_mid);
  gen_ops(seed, K, ops, states, mids, has_mid);
  SimDisk d;
  sim_init(&d, NULL, 0);
  d.crash_at = N;
  fk_io io = sim_io(&d);
  fkx *x = NULL;
  fkx_options o = mkopt();
  int acked = 0, inflight = 0;
  int rc = fkx_open_io(&io, &o, &x);
  long ev_work = 0;
  if (rc == 0) {
    for (int i = 0; i < K; i++) {
      rc = run_op(x, &ops[i]);
      ev_work = d.events;
      if (rc) { inflight = 1; break; }
      acked++;
      if (N < 0) {
        char buf[200];
        if (!view_equal(x, &states[i + 1]) || fkx_check(x, buf, sizeof buf)) { printf("  VIOLATION seed=%llu op %d (kind %d) changed the visible state wrongly: %s %s\n", (unsigned long long)seed, i, ops[i].kind, why, buf); st->violations++; }
      }
    }
    if (rc && N < 0) { printf("  VIOLATION seed=%llu op failed without a power cut: rc=%d %s\n", (unsigned long long)seed, rc, fkx_errmsg(x)); st->violations++; }
    fkx_close(x);
  } else if (N < 0) { printf("  VIOLATION seed=%llu open failed rc=%d\n", (unsigned long long)seed, rc); st->violations++; }
  if (events_out) *events_out = ev_work;
  Ctx c;
  memset(&c, 0, sizeof c);
  c.st = st; c.seed = seed; c.N = N; c.tag = "power cut"; c.mid_idx = -1; c.undo_op_br = -1;
  c.cand[c.ncand++] = &states[acked];
  if (inflight && acked < K) {
    c.cand[c.ncand++] = &states[acked + 1];
    if (has_mid[acked + 1]) {
      c.cand[c.ncand++] = &mids[acked + 1];
      if (ops[acked].kind == 6) { c.mid_idx = c.ncand - 1; c.after_undo = &states[acked + 1]; c.undo_op_br = ops[acked].br; }
    }
  }
  st->cases++;
  Rng r;
  rng_seed(&r, seed * 31 + (uint64_t)N * 17 + 3);
  c.rr = &r;
  sim_images(&d, &r, top, &c);
  sim_free(&d);
  free(states);
  free(mids);
}

int main(int argc, char **argv) {
  int seeds = argc > 1 ? atoi(argv[1]) : 20, K = argc > 2 ? atoi(argv[2]) : 12;
  Stats st;
  memset(&st, 0, sizeof st);
  long total_events = 0;
  for (int s = 1; s <= seeds; s++) {
    long ev = 0;
    run_case((uint64_t)s, K, -1, &st, &ev);
    total_events += ev;
    for (long n = 0; n < ev + 1; n++) run_case((uint64_t)s, K, n, &st, NULL);
  }
  printf("seeds=%d ops=%d cut points=%ld cases=%ld images=%ld recoveries cut again=%ld\n", seeds, K, total_events, st.cases, st.images, st.nested);
  printf("TOTAL VIOLATIONS=%ld\n", st.violations);
  return st.violations ? 1 : 0;
}
