/* Power-loss test of the fkx session driver: promote by replay (and by fast-forward) and undo (exact and by replay), with real SQLite tables
 * through the VFS on the simulated disk (test/simdisk.c). Random workloads of row writes, forks, promotes, undos and discards; the power is cut
 * before every I/O event; every post-crash image (any subset of the pending writes, torn) is opened with fkx. After each cut:
 *   - fkx opens and its index is consistent (fkx_check);
 *   - the visible state (main's rows, how many undo points it lists, which forks exist and their rows) is exactly one the operation in flight could
 *     have left: before, after, or the one in between. A replay promote changes main in one SQLite commit, and the undo point is listed after it, so
 *     "promoted, not yet listed" is allowed; an undo by reset is followed by removing the entry, so "restored, still listed" is allowed (and a second
 *     undo finishes it); an undo by replay removes its entry first, so "entry removed, not yet undone" is allowed (it is not repeatable, and is safe);
 *   - gc finds nothing more to collect, a new fork and its discard work, and a reopen shows the same;
 *   - some images are recovered with a second power cut in the middle of the recovery.
 * Forks write ids in a range of their own, so replays never conflict. Build with the session SQLite (see scripts/test-fkx.sh). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fkx/fkx_internal.h"
#include "simdisk.h"

#define KEEP 2
#define NS 4 /* slot 0 is main, 1 to 3 are forks */
#define NK 8
#define MAXOPS 24

typedef struct { uint8_t has; int v; } Cell;
typedef struct { Cell c[NS * NK]; } Rows;
typedef struct { Rows before, after; uint64_t bstate, astate; } Undo;
typedef struct { int alive, promoted; Rows rows, base; uint64_t state, bstate; } Fk;
typedef struct { int table; Rows main; uint64_t mstate, ctr; Undo undo[KEEP]; int nundo; Fk f[NS]; } Model;
/* kinds: 0 setup, 1 main set, 2 main delete, 3 fork, 4 fork set, 5 fork delete, 6 promote, 7 undo, 8 discard */
typedef struct { int kind, slot, k, v; } Op;

static const char *fname(int s) { static char b[NS][4]; snprintf(b[s], sizeof b[s], "f%d", s); return b[s]; }
static int cell_id(int e) { return (e / NK) * 100 + e % NK + 1; }
static int cell_of(int slot, int k) { return slot * NK + (k - 1); }

static void apply_cell(Rows *r, int e, const Cell *from) { r->c[e] = *from; }

static int g_count;
static long n_ff, n_replay, n_uptodate, n_undo_exact, n_undo_replay;
/* mid: the view after the first visible change of a promote or an undo, before the undo list changes. exact: an undo that is a reset. */
static int m_apply(Model *M, const Op *o, Model *mid, int *exact) {
  int has_mid = 0;
  *exact = 0;
  switch (o->kind) {
    case 0: M->table = 1; M->mstate = ++M->ctr; break;
    case 1: M->main.c[cell_of(0, o->k)].has = 1; M->main.c[cell_of(0, o->k)].v = o->v; M->mstate = ++M->ctr; break;
    case 2: M->main.c[cell_of(0, o->k)].has = 0; M->mstate = ++M->ctr; break;
    case 3: { Fk *f = &M->f[o->slot]; memset(f, 0, sizeof *f); f->alive = 1; f->rows = M->main; f->base = M->main; f->state = M->mstate; f->bstate = M->mstate; break; }
    case 4: { Fk *f = &M->f[o->slot]; f->rows.c[cell_of(o->slot, o->k)].has = 1; f->rows.c[cell_of(o->slot, o->k)].v = o->v; f->state = ++M->ctr; break; }
    case 5: { Fk *f = &M->f[o->slot]; f->rows.c[cell_of(o->slot, o->k)].has = 0; f->state = ++M->ctr; break; }
    case 6: {
      Fk *f = &M->f[o->slot];
      if (M->mstate == f->state) { if (g_count) n_uptodate++; break; } /* up to date: nothing is written */
      Rows old = M->main;
      uint64_t ost = M->mstate;
      if (M->mstate == f->bstate) { M->main = f->rows; M->mstate = f->state; if (g_count) n_ff++; }
      else {
        if (g_count) n_replay++;
        for (int e = 0; e < NS * NK; e++) if (memcmp(&f->rows.c[e], &f->base.c[e], sizeof(Cell))) apply_cell(&M->main, e, &f->rows.c[e]);
        if (memcmp(&M->main, &old, sizeof old)) M->mstate = ++M->ctr; /* a replay that changes no row writes nothing: no commit, the state id stays */
      }
      *mid = *M;
      has_mid = 1;
      for (int i = KEEP - 1; i > 0; i--) M->undo[i] = M->undo[i - 1];
      M->undo[0].before = old; M->undo[0].bstate = ost; M->undo[0].after = M->main; M->undo[0].astate = M->mstate;
      if (M->nundo < KEEP) M->nundo++;
      f->promoted = 1;
      break;
    }
    case 7: {
      Undo u = M->undo[0];
      *mid = *M;
      has_mid = 1;
      if (g_count) { if (M->mstate == u.astate) n_undo_exact++; else n_undo_replay++; }
      if (M->mstate == u.astate) { *exact = 1; M->main = u.before; M->mstate = u.bstate; mid->main = u.before; mid->mstate = u.bstate; }
      else {
        Rows was = M->main;
        for (int e = 0; e < NS * NK; e++) if (memcmp(&u.after.c[e], &u.before.c[e], sizeof(Cell))) apply_cell(&M->main, e, &u.before.c[e]);
        if (memcmp(&M->main, &was, sizeof was)) M->mstate = ++M->ctr;
        mid->nundo = M->nundo - 1;
      }
      for (int i = 1; i < KEEP; i++) M->undo[i - 1] = M->undo[i];
      M->nundo--;
      break;
    }
    case 8: memset(&M->f[o->slot], 0, sizeof(Fk)); break;
    default: break;
  }
  return has_mid;
}

static void gen_ops(uint64_t seed, int K, Op *ops, Model *states, Model *mids, int *has_mid, int *exact) {
  Rng r;
  rng_seed(&r, seed);
  memset(&states[0], 0, sizeof states[0]);
  int vctr = 0;
  Op first = {0, 0, 0, 0};
  ops[0] = first;
  states[1] = states[0];
  has_mid[1] = m_apply(&states[1], &first, &mids[1], &exact[1]);
  for (int i = 1; i < K; i++) {
    Model *M = &states[i];
    Op o;
    int tries = 0;
    for (;;) {
      memset(&o, 0, sizeof o);
      uint32_t roll = rng_below(&r, 100);
      int alive[NS], na = 0, freeS[NS], nf = 0, prom[NS], np = 0;
      for (int s = 1; s < NS; s++) { if (M->f[s].alive) { alive[na++] = s; if (!M->f[s].promoted && M->f[s].state != M->mstate) prom[np++] = s; } else freeS[nf++] = s; }
      /* every write must really change something: a delete of a row that is not there is no commit at all, so the state id would not move */
      if (roll < 22) { o.kind = rng_below(&r, 5) ? 1 : 2; o.k = 1 + (int)rng_below(&r, NK); o.v = ++vctr; if (o.kind == 2 && !M->main.c[cell_of(0, o.k)].has) o.kind = 1; break; }
      if (roll < 40 && nf) { o.kind = 3; o.slot = freeS[rng_below(&r, (uint32_t)nf)]; break; }
      if (roll < 62 && na) { o.kind = rng_below(&r, 5) ? 4 : 5; o.slot = alive[rng_below(&r, (uint32_t)na)]; o.k = 1 + (int)rng_below(&r, NK); o.v = ++vctr; if (o.kind == 5 && !M->f[o.slot].rows.c[cell_of(o.slot, o.k)].has) o.kind = 4; break; }
      if (roll < 82 && np) { o.kind = 6; o.slot = prom[rng_below(&r, (uint32_t)np)]; break; }
      if (roll < 94 && M->nundo > 0) { o.kind = 7; break; }
      if (roll < 100 && na) { o.kind = 8; o.slot = alive[rng_below(&r, (uint32_t)na)]; break; }
      if (++tries > 30) { o.kind = 1; o.k = 1; o.v = ++vctr; break; }
    }
    ops[i] = o;
    states[i + 1] = states[i];
    has_mid[i + 1] = m_apply(&states[i + 1], &o, &mids[i + 1], &exact[i + 1]);
  }
}

/* ---------- the system under test ---------- */
static char why[300];
static void why_prefix(const char *pre) { char t[300]; snprintf(t, sizeof t, "%s", why); snprintf(why, sizeof why, "%.40s: %.240s", pre, t); }

static int sql(fkx *x, const char *ref, const char *q) {
  sqlite3 *db = NULL;
  int rc = fkx_open_db(x, ref, SQLITE_OPEN_READWRITE, &db);
  if (rc) return rc;
  rc = sqlite3_exec(db, q, 0, 0, 0);
  sqlite3_close(db);
  return rc;
}
/* the rows of table t as "id=v;..." or "!" when it cannot be read */
static const char *rows_text(fkx *x, const char *ref) {
  static char out[2][1024];
  static int w;
  char *o = out[w ^= 1];
  strcpy(o, "!");
  sqlite3 *db = NULL;
  if (fkx_open_db(x, ref, SQLITE_OPEN_READONLY, &db)) return o;
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "SELECT id, v FROM t ORDER BY id", -1, &st, 0) == SQLITE_OK) {
    size_t n = 0;
    o[0] = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) n += (size_t)snprintf(o + n, sizeof out[0] - n, "%d=%d;", sqlite3_column_int(st, 0), sqlite3_column_int(st, 1));
    if (rc != SQLITE_DONE) strcpy(o, "!");
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  return o;
}
static const char *model_text(const Rows *r, int table) {
  static char out[2][1024];
  static int w;
  char *o = out[w ^= 1];
  if (!table) { strcpy(o, "!"); return o; }
  size_t n = 0;
  o[0] = 0;
  for (int e = 0; e < NS * NK; e++) if (r->c[e].has) n += (size_t)snprintf(o + n, sizeof out[0] - n, "%d=%d;", cell_id(e), r->c[e].v);
  return o;
}

static int view_equal(fkx *x, const Model *M) {
  fkx_info in;
  if (!M->table) {
    /* before the table was made: main is there and has no table; no forks */
    if (strcmp(rows_text(x, "main"), "!")) { snprintf(why, sizeof why, "main has a table before it was created"); return 0; }
    size_t n = 0;
    fkx_list(x, 0, NULL, 0, &n);
    if (n != 1) { snprintf(why, sizeof why, "%zu named branches before any fork", n); return 0; }
    return 1;
  }
  const char *a = rows_text(x, "main"), *b = model_text(&M->main, 1);
  if (strcmp(a, b)) { snprintf(why, sizeof why, "main holds '%.100s', expected '%.100s'", a, b); return 0; }
  if (fkx_info_get(x, "main", &in) || (int)in.n_undo != M->nundo) { snprintf(why, sizeof why, "main lists %u undo points, expected %d", in.n_undo, M->nundo); return 0; }
  int na = 1;
  for (int s = 1; s < NS; s++) {
    int rc = fkx_info_get(x, fname(s), &in);
    if (!M->f[s].alive) { if (rc == 0) { snprintf(why, sizeof why, "fork %s exists but should not", fname(s)); return 0; } continue; }
    na++;
    if (rc) { snprintf(why, sizeof why, "fork %s is missing", fname(s)); return 0; }
    a = rows_text(x, fname(s));
    b = model_text(&M->f[s].rows, 1);
    if (strcmp(a, b)) { snprintf(why, sizeof why, "fork %s holds '%.100s', expected '%.100s'", fname(s), a, b); return 0; }
  }
  size_t n = 0;
  fkx_list(x, 0, NULL, 0, &n);
  if ((int)n != na) { snprintf(why, sizeof why, "%zu named branches, expected %d", n, na); return 0; }
  return 1;
}

static int run_op(fkx *x, const Op *o) {
  char q[160];
  switch (o->kind) {
    case 0: return sql(x, "main", "CREATE TABLE t(id INTEGER PRIMARY KEY, v INT)");
    case 1: snprintf(q, sizeof q, "INSERT OR REPLACE INTO t VALUES(%d,%d)", o->k, o->v); return sql(x, "main", q);
    case 2: snprintf(q, sizeof q, "DELETE FROM t WHERE id=%d", o->k); return sql(x, "main", q);
    case 3: return fkx_fork(x, "main", fname(o->slot));
    case 4: snprintf(q, sizeof q, "INSERT OR REPLACE INTO t VALUES(%d,%d)", o->slot * 100 + o->k, o->v); return sql(x, fname(o->slot), q);
    case 5: snprintf(q, sizeof q, "DELETE FROM t WHERE id=%d", o->slot * 100 + o->k); return sql(x, fname(o->slot), q);
    case 6: { fkx_promote_result r; int rc = fkx_promote(x, fname(o->slot), NULL, &r); fkx_promote_result_free(&r); return rc; }
    case 7: { fkx_promote_result r; int rc = fkx_undo(x, "main", &r); fkx_promote_result_free(&r); return rc; }
    default: return fkx_discard(x, fname(o->slot));
  }
}

typedef struct { long violations, cases, images, nested; } Stats;
typedef struct {
  const Model *cand[5]; int ncand; int mid_idx; const Model *after_undo;
  Stats *st; uint64_t seed; long N; const char *tag; long counter;
} Ctx;

static void report(Ctx *c, const char *what) {
  c->st->violations++;
  printf("  VIOLATION seed=%llu cut=%ld (%s) %s: %s\n", (unsigned long long)c->seed, c->N, c->tag, what, why);
}
static fkx_options mkopt(void) { fkx_options o; fkx_options_default(&o); o.undo_keep = KEEP; o.prealloc_chunk = 0; return o; }

static void leaf(void *vc, const uint8_t *img, uint64_t len);

static void nested_cuts(Ctx *c, const uint8_t *img, uint64_t len) {
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
    c2.tag = "cut during recovery";
    c->st->nested++;
    sim_images(&d2, &r, leaf, &c2);
    sim_free(&d2);
  }
}

static void verify(Ctx *c, const uint8_t *img, uint64_t len, int deep) {
  c->st->images++;
  SimDisk d;
  sim_init(&d, img, len);
  fk_io io = sim_io(&d);
  fkx *x = NULL;
  fkx_options o = mkopt();
  int rc = fkx_open_io(&io, &o, &x);
  if (rc) { snprintf(why, sizeof why, "fkx_open failed rc=%d", rc); report(c, "recovery failed"); sim_free(&d); return; }
  char buf[200];
  int which = -1;
  if (fkx_check(x, buf, sizeof buf)) { snprintf(why, sizeof why, "%s", buf); report(c, "the index is inconsistent after recovery"); }
  else {
    int ok = 0;
    char first[200] = "";
    for (int k = 0; k < c->ncand && !ok; k++) { if (view_equal(x, c->cand[k])) { ok = 1; which = k; } else if (!first[0]) snprintf(first, sizeof first, "%.190s", why); }
    if (!ok) { snprintf(why, sizeof why, "%s (not one of the %d states the operation could leave)", first, c->ncand); report(c, "the visible state is wrong"); }
    else if (deep) {
      fkx_gc_report g;
      if (fkx_gc(x, &g) || g.orphan_bases || g.orphan_undo || g.unlisted_undo || g.over_retention) { snprintf(why, sizeof why, "gc after recovery found garbage again (%u %u %u %u)", g.orphan_bases, g.orphan_undo, g.unlisted_undo, g.over_retention); report(c, "recovery left garbage"); }
      if (c->mid_idx >= 0 && which == c->mid_idx) {
        fkx_promote_result r;
        int urc = fkx_undo(x, "main", &r);
        fkx_promote_result_free(&r);
        if (urc != 0 || !view_equal(x, c->after_undo)) { why_prefix("finishing a cut undo"); report(c, "undo did not finish"); }
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
        const Model *want = which == -2 ? c->after_undo : c->cand[which];
        if (fkx_open_io(&io2, &o2, &y) || fkx_check(y, buf, sizeof buf) || !view_equal(y, want)) { why_prefix("after one more operation and a reopen"); report(c, "state lost"); }
        if (y) fkx_close(y);
        sim_free(&d2);
      }
    }
  }
  if (x) fkx_close(x);
  sim_free(&d);
}
static void leaf(void *vc, const uint8_t *img, uint64_t len) { verify((Ctx *)vc, img, len, 0); }
static void top(void *vc, const uint8_t *img, uint64_t len) {
  Ctx *c = vc;
  verify(c, img, len, 1);
  if (++c->counter % 5 == 0) nested_cuts(c, img, len);
}

static void run_case(uint64_t seed, int K, long N, Stats *st, long *events_out) {
  Op ops[MAXOPS];
  Model *states = calloc((size_t)(MAXOPS + 1), sizeof *states), *mids = calloc((size_t)(MAXOPS + 1), sizeof *mids);
  int has_mid[MAXOPS + 1], exact[MAXOPS + 1];
  memset(has_mid, 0, sizeof has_mid);
  memset(exact, 0, sizeof exact);
  g_count = N < 0 && !events_out ? 0 : (N < 0);
  gen_ops(seed, K, ops, states, mids, has_mid, exact);
  g_count = 0;
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
        if (!view_equal(x, &states[i + 1]) || fkx_check(x, buf, sizeof buf)) {
          printf("  VIOLATION seed=%llu op %d (kind %d) changed the visible state wrongly: %s\n", (unsigned long long)seed, i, ops[i].kind, why);
          if (getenv("FKX_TRACE")) for (int j = 0; j <= i; j++) printf("    op %d: kind %d slot %d k %d v %d\n", j, ops[j].kind, ops[j].slot, ops[j].k, ops[j].v);
          st->violations++;
        }
      }
    }
    if (rc && N < 0) { printf("  VIOLATION seed=%llu op %d (kind %d) failed without a power cut: rc=%d %s\n", (unsigned long long)seed, acked, ops[acked].kind, rc, fkx_errmsg(x)); st->violations++; }
    fkx_close(x);
  } else if (N < 0) { printf("  VIOLATION seed=%llu open failed rc=%d\n", (unsigned long long)seed, rc); st->violations++; }
  if (events_out) *events_out = ev_work;
  Ctx c;
  memset(&c, 0, sizeof c);
  c.st = st; c.seed = seed; c.N = N; c.tag = "power cut"; c.mid_idx = -1;
  c.cand[c.ncand++] = &states[acked];
  if (inflight && acked < K) {
    c.cand[c.ncand++] = &states[acked + 1];
    if (has_mid[acked + 1]) {
      c.cand[c.ncand++] = &mids[acked + 1];
      if (ops[acked].kind == 7 && exact[acked + 1]) { c.mid_idx = c.ncand - 1; c.after_undo = &states[acked + 1]; }
    }
  }
  st->cases++;
  Rng r;
  rng_seed(&r, seed * 31 + (uint64_t)N * 17 + 3);
  sim_images(&d, &r, top, &c);
  sim_free(&d);
  free(states);
  free(mids);
}

int main(int argc, char **argv) {
  int seeds = argc > 1 ? atoi(argv[1]) : 10, K = argc > 2 ? atoi(argv[2]) : 12;
  if (K > MAXOPS) K = MAXOPS;
  setvbuf(stdout, NULL, _IOLBF, 0);
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
  printf("promotes: %ld fast-forward, %ld replay, %ld up to date; undos: %ld exact, %ld by replay\n", n_ff, n_replay, n_uptodate, n_undo_exact, n_undo_replay);
  printf("TOTAL VIOLATIONS=%ld\n", st.violations);
  return st.violations ? 1 : 0;
}
