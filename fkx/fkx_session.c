/* fkx session driver: diff, replay promote, undo by replay, and the stepwise primitives, on SQLite's session extension.
 * Needs a SQLite built with SQLITE_ENABLE_SESSION and SQLITE_ENABLE_PREUPDATE_HOOK. Replaces fkx_nosession.c. See docs/FKX-DESIGN.md
 * (Promote, in detail) and docs/SESSION-DESIGN.md (the schema policy, and the experiments X1 to X6 behind it). */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"
#include "sqlite3session.h"
#include "fkx_internal.h"

#if !defined(SQLITE_ENABLE_SESSION) || !defined(SQLITE_ENABLE_PREUPDATE_HOOK)
#error "fkx_session.c needs SQLITE_ENABLE_SESSION and SQLITE_ENABLE_PREUPDATE_HOOK"
#endif

int fkx_have_session(void) { return 1; }

/* ---------- small SQLite helpers ---------- */
static int sq_exec(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, 0, 0, NULL); }
static int sq_execf(sqlite3 *db, const char *fmt, const char *a, const char *b) {
  char *q = sqlite3_mprintf(fmt, a, b);
  if (!q) return SQLITE_NOMEM;
  int rc = sq_exec(db, q);
  sqlite3_free(q);
  return rc;
}
static long sq_long(sqlite3 *db, const char *sql) {
  sqlite3_stmt *st = NULL;
  long v = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(st) == SQLITE_ROW) v = (long)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

/* ---------- schema objects and table classification ---------- */
typedef struct { char *type, *name, *tbl, *sql; } Obj;
typedef struct { Obj *o; size_t n; } Objs;

static void objs_free(Objs *s) {
  for (size_t i = 0; i < s->n; i++) { free(s->o[i].type); free(s->o[i].name); free(s->o[i].tbl); free(s->o[i].sql); }
  free(s->o);
  memset(s, 0, sizeof *s);
}
static int objs_load(sqlite3 *db, const char *schema, Objs *s) {
  memset(s, 0, sizeof *s);
  char *q = sqlite3_mprintf("SELECT type, name, tbl_name, sql FROM \"%w\".sqlite_schema WHERE name NOT LIKE 'sqlite\\_%%' ESCAPE '\\' ORDER BY type, name", schema);
  if (!q) return SQLITE_NOMEM;
  sqlite3_stmt *st = NULL;
  int rc = sqlite3_prepare_v2(db, q, -1, &st, NULL);
  sqlite3_free(q);
  if (rc != SQLITE_OK) return rc;
  size_t cap = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    if (s->n == cap) { cap = cap ? cap * 2 : 16; Obj *g = realloc(s->o, cap * sizeof *g); if (!g) { sqlite3_finalize(st); objs_free(s); return SQLITE_NOMEM; } s->o = g; }
    Obj *o = &s->o[s->n++];
    const unsigned char *a = sqlite3_column_text(st, 0), *b = sqlite3_column_text(st, 1), *c = sqlite3_column_text(st, 2), *d = sqlite3_column_text(st, 3);
    o->type = strdup(a ? (const char *)a : ""); o->name = strdup(b ? (const char *)b : ""); o->tbl = strdup(c ? (const char *)c : ""); o->sql = strdup(d ? (const char *)d : "");
  }
  sqlite3_finalize(st);
  return SQLITE_OK;
}
static int objs_equal(const Objs *a, const Objs *b) {
  if (a->n != b->n) return 0;
  for (size_t i = 0; i < a->n; i++)
    if (strcmp(a->o[i].type, b->o[i].type) || strcmp(a->o[i].name, b->o[i].name) || strcmp(a->o[i].sql, b->o[i].sql)) return 0;
  return 1;
}
static const Obj *objs_table(const Objs *s, const char *name) {
  for (size_t i = 0; i < s->n; i++) if (!strcmp(s->o[i].type, "table") && !strcmp(s->o[i].name, name)) return &s->o[i];
  return NULL;
}
static int is_virtual(const Obj *o) { return o && strncasecmp(o->sql, "CREATE VIRTUAL", 14) == 0; }
/* a shadow table of a virtual table: its name starts with the virtual table's name and an underscore */
static int is_shadow(const Objs *s, const char *name) {
  for (size_t i = 0; i < s->n; i++) {
    if (!is_virtual(&s->o[i]) || strcmp(s->o[i].type, "table")) continue;
    size_t l = strlen(s->o[i].name);
    if (strncmp(name, s->o[i].name, l) == 0 && name[l] == '_') return 1;
  }
  return 0;
}

/* a signature of a table's columns: name, type, not null, default, primary key position, hidden; and whether it has a primary key */
static char *xinfo_sig(sqlite3 *db, const char *schema, const char *table, int *has_pk) {
  char *q = sqlite3_mprintf("PRAGMA \"%w\".table_xinfo(\"%w\")", schema, table);
  if (!q) return NULL;
  sqlite3_stmt *st = NULL;
  int rc = sqlite3_prepare_v2(db, q, -1, &st, NULL);
  sqlite3_free(q);
  if (rc != SQLITE_OK) return NULL;
  sqlite3_str *s = sqlite3_str_new(db);
  int pk = 0, rows = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    rows++;
    const unsigned char *nm = sqlite3_column_text(st, 1), *ty = sqlite3_column_text(st, 2), *df = sqlite3_column_text(st, 4);
    sqlite3_str_appendf(s, "%d|%s|%s|%d|%s|%d|%d;", sqlite3_column_int(st, 0), nm ? (const char *)nm : "", ty ? (const char *)ty : "", sqlite3_column_int(st, 3), df ? (const char *)df : "(null)", sqlite3_column_int(st, 5), sqlite3_column_int(st, 6));
    if (sqlite3_column_int(st, 5) > 0) pk = 1;
  }
  sqlite3_finalize(st);
  char *z = sqlite3_str_finish(s);
  char *out = z ? strdup(z) : NULL;
  sqlite3_free(z);
  if (has_pk) *has_pk = pk;
  if (!rows) { free(out); return NULL; }
  return out;
}

/* 1 if the rows of table t differ between schemas a and b (same columns); a failure counts as different */
static int rows_differ(sqlite3 *db, const char *a, const char *b, const char *t) {
  char *q = sqlite3_mprintf("SELECT (SELECT count(*) FROM \"%w\".\"%w\") <> (SELECT count(*) FROM \"%w\".\"%w\")"
                            " OR EXISTS (SELECT * FROM \"%w\".\"%w\" EXCEPT SELECT * FROM \"%w\".\"%w\")"
                            " OR EXISTS (SELECT * FROM \"%w\".\"%w\" EXCEPT SELECT * FROM \"%w\".\"%w\")",
                            a, t, b, t, a, t, b, t, b, t, a, t);
  if (!q) return 1;
  long v = sq_long(db, q);
  sqlite3_free(q);
  return v != 0;
}

static int tdiff_cmp(const void *a, const void *b) { return strcmp(((const fkx_table_diff *)a)->name, ((const fkx_table_diff *)b)->name); }

/* Classify every table of either schema (a = the branch being looked at, b = the one it is compared with): nothing is left out silently. */
static int classify(sqlite3 *db, const char *a, const char *b, fkx_table_diff **out, size_t *n, int *schema_differs) {
  Objs sa, sb;
  int rc;
  *out = NULL; *n = 0;
  if ((rc = objs_load(db, a, &sa)) != SQLITE_OK) return rc;
  if ((rc = objs_load(db, b, &sb)) != SQLITE_OK) { objs_free(&sa); return rc; }
  if (schema_differs) {
    *schema_differs = !objs_equal(&sa, &sb);
#ifdef FKX_BUG_POLICY_IGNORES_DDL
    *schema_differs = 0; /* mutant: a change of the schema is not noticed */
#endif
  }
  size_t cap = sa.n + sb.n;
  fkx_table_diff *t = calloc(cap ? cap : 1, sizeof *t);
  if (!t) { objs_free(&sa); objs_free(&sb); return SQLITE_NOMEM; }
  size_t k = 0;
  for (int side = 0; side < 2; side++) {
    const Objs *s = side ? &sb : &sa, *o = side ? &sa : &sb;
    for (size_t i = 0; i < s->n; i++) {
      if (strcmp(s->o[i].type, "table")) continue;
      const char *name = s->o[i].name;
      if (side && objs_table(o, name)) continue; /* both: already done from side a */
      fkx_table_diff *d = &t[k++];
      d->name = strdup(name);
      const Obj *other = objs_table(o, name);
      if (!other) { d->status = side ? FKX_TABLE_ONLY_THERE : FKX_TABLE_ONLY_HERE; continue; }
      if (is_virtual(&s->o[i]) || is_virtual(other) || is_shadow(&sa, name) || is_shadow(&sb, name)) {
        d->status = FKX_TABLE_VIRTUAL;
        d->rows_differ = rows_differ(db, a, b, name);
        continue;
      }
      int pa = 0, pb = 0;
      char *xa = xinfo_sig(db, a, name, &pa), *xb = xinfo_sig(db, b, name, &pb);
      int same = xa && xb && strcmp(xa, xb) == 0;
      free(xa); free(xb);
      if (!same) { d->status = FKX_TABLE_SCHEMA; continue; }
      if (!pa) { d->status = FKX_TABLE_NO_PK; d->rows_differ = rows_differ(db, a, b, name); continue; }
      d->status = FKX_TABLE_COMPARED;
    }
  }
  objs_free(&sa);
  objs_free(&sb);
  qsort(t, k, sizeof *t, tdiff_cmp);
  *out = t; *n = k;
  return SQLITE_OK;
}
static void tables_free(fkx_table_diff *t, size_t n) {
  for (size_t i = 0; i < n; i++) free(t[i].name);
  free(t);
}

/* ---------- diff ---------- */
struct fkx_diff_handle {
  fkx *x;
  sqlite3 *db;
  fkx_table_diff *tables;
  size_t n;
  int schema_differs;
};

static void diff_close_impl(fkx_diff_handle *h) {
  if (!h) return;
  fkx_i_untrack(h->x, h);
  if (h->db) { sq_exec(h->db, "ROLLBACK"); sqlite3_close(h->db); }
  tables_free(h->tables, h->n);
  free(h);
}
static void diff_abort_cb(void *p) { diff_close_impl(p); }
void fkx_diff_close(fkx_diff_handle *h) { diff_close_impl(h); }

int fkx_diff_open(fkx *x, const char *ref, const char *against, fkx_diff_handle **hout, sqlite3 **db, const fkx_table_diff **tables, size_t *ntables, int *schema_differs) {
  uint32_t id, oid;
  Meta m, om;
  int rc;
  if ((rc = fkx_i_resolve(x, ref, &id, &m))) return rc;
  if (against) { if ((rc = fkx_i_resolve(x, against, &oid, &om))) return rc; }
  else {
    if (m.kind != FKX_FORK || !fkx_i_find_uid(x, m.base, &oid, &om)) return fkx_i_fail(x, FKX_EINVAL, "'%s' has no base to compare with: name the branch to compare with", ref);
  }
  fkx_diff_handle *h = calloc(1, sizeof *h);
  if (!h) return FKX_ENOMEM;
  h->x = x;
  if ((rc = fkx_open_db(x, ref, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, &h->db))) { free(h); return rc; }
  char o_ref[24];
  fkx_i_ref(om.uid, o_ref);
  /* ATTACH takes the same open flags as the main database; mode=ro keeps a read-only branch from being refused for write */
  char *q = sqlite3_mprintf("ATTACH 'file:b%u?mode=ro' AS other", (unsigned)oid);
  rc = q ? sq_exec(h->db, q) : SQLITE_NOMEM;
  sqlite3_free(q);
  if (rc == SQLITE_OK) rc = sq_exec(h->db, "BEGIN");
  if (rc == SQLITE_OK) rc = classify(h->db, "main", "other", &h->tables, &h->n, &h->schema_differs);
  if (rc != SQLITE_OK) {
    const char *msg = sqlite3_errmsg(h->db);
    rc = fkx_i_fail(x, FKX_ESQLITE, "comparing '%s' with '%s' failed: %s", ref, against ? against : "its base", msg);
    diff_close_impl(h);
    return rc;
  }
  if ((rc = fkx_i_track(x, h, diff_abort_cb))) { diff_close_impl(h); return rc; }
  *hout = h;
  if (db) *db = h->db;
  if (tables) *tables = h->tables;
  if (ntables) *ntables = h->n;
  if (schema_differs) *schema_differs = h->schema_differs;
  return FKX_OK;
}

static int count_changes(int n, void *cs, fkx_table_diff *t, size_t nt) {
  sqlite3_changeset_iter *it = NULL;
  if (sqlite3changeset_start(&it, n, cs) != SQLITE_OK) return -1;
  while (sqlite3changeset_next(it) == SQLITE_ROW) {
    const char *tab;
    int nc, op, ind;
    sqlite3changeset_op(it, &tab, &nc, &op, &ind);
    for (size_t i = 0; i < nt; i++)
      if (!strcmp(t[i].name, tab)) { if (op == SQLITE_INSERT) t[i].inserts++; else if (op == SQLITE_DELETE) t[i].deletes++; else t[i].updates++; break; }
  }
  sqlite3changeset_finalize(it);
  return 0;
}

int fkx_diff(fkx *x, const char *ref, const char *against, fkx_diff_result *out) {
  fkx_diff_handle *h = NULL;
  memset(out, 0, sizeof *out);
  int rc = fkx_diff_open(x, ref, against, &h, NULL, NULL, NULL, NULL);
  if (rc) return rc;
  sqlite3_session *ss = NULL;
  if (sqlite3session_create(h->db, "main", &ss) != SQLITE_OK) { fkx_diff_close(h); return fkx_i_fail(x, FKX_ESQLITE, "creating a session failed"); }
  for (size_t i = 0; i < h->n; i++) {
    if (h->tables[i].status != FKX_TABLE_COMPARED) continue;
    char *err = NULL;
    if (sqlite3session_attach(ss, h->tables[i].name) != SQLITE_OK || sqlite3session_diff(ss, "other", h->tables[i].name, &err) != SQLITE_OK) {
      rc = fkx_i_fail(x, FKX_ESQLITE, "diffing table '%s' failed: %s", h->tables[i].name, err ? err : "?");
      sqlite3_free(err);
      sqlite3session_delete(ss);
      fkx_diff_close(h);
      return rc;
    }
  }
  int n = 0;
  void *cs = NULL;
  if (sqlite3session_changeset(ss, &n, &cs) != SQLITE_OK) { sqlite3session_delete(ss); fkx_diff_close(h); return fkx_i_fail(x, FKX_ESQLITE, "reading the changeset failed"); }
  sqlite3session_delete(ss);
  out->tables = h->tables;
  out->ntables = h->n;
  out->schema_differs = h->schema_differs;
  h->tables = NULL;
  h->n = 0;
  count_changes(n, cs, out->tables, out->ntables);
  out->changeset = malloc(n ? (size_t)n : 1);
  if (out->changeset && n > 0) memcpy(out->changeset, cs, (size_t)n);
  out->nchangeset = n;
  sqlite3_free(cs);
  fkx_diff_close(h);
  return out->changeset ? FKX_OK : FKX_ENOMEM;
}

/* ---------- replay ---------- */
struct fkx_replay {
  fkx *x;
  sqlite3 *db;
  uint32_t pid;
  Meta pm;
  uint64_t ub;      /* the undo "before" snapshot, 0 if none */
  int take_undo;
  char **trig;      /* the sql of the triggers dropped on the touched tables */
  size_t ntrig;
  int recorded;
};

static void replay_free(fkx_replay *r) {
  if (!r) return;
  for (size_t i = 0; i < r->ntrig; i++) free(r->trig[i]);
  free(r->trig);
  free(r);
}
static void replay_abort_impl(fkx_replay *r) {
  if (!r) return;
  fkx_i_untrack(r->x, r);
  if (r->db) { sq_exec(r->db, "ROLLBACK"); sqlite3_close(r->db); r->db = NULL; } /* the rollback also puts the dropped triggers back */
  if (r->ub) (void)fkx_i_discard_uid(r->x, r->ub);
  replay_free(r);
}
static void replay_abort_cb(void *p) { replay_abort_impl(p); }
void fkx_replay_abort(fkx_replay *r) { replay_abort_impl(r); }

static int refuse(fkx *x, char **refusal, const char *fmt, const char *a, const char *b) {
  char buf[400];
  snprintf(buf, sizeof buf, fmt, a ? a : "", b ? b : "");
  if (refusal) *refusal = strdup(buf);
  return fkx_i_fail(x, FKX_EREFUSED, "%s", buf);
}

/* The schema policy (docs/SESSION-DESIGN.md): what a changeset cannot carry, or would carry wrongly, is refused before anything is applied. */
static int check_policy(fkx *x, sqlite3 *db, const char *const *touched, size_t ntouched, const char *target_name, char **refusal) {
  fkx_table_diff *t = NULL;
  size_t n = 0;
  int sd = 0;
  if (classify(db, "new", "old", &t, &n, &sd) != SQLITE_OK) return fkx_i_fail(x, FKX_ESQLITE, "reading the schemas failed: %s", sqlite3_errmsg(db));
  int rc = FKX_OK;
  if (sd) rc = refuse(x, refusal, "the fork changed the schema (a table, index, view or trigger was created, altered or dropped): a changeset cannot carry that, and %s", "a fast-forward is the only way to promote it", NULL);
  for (size_t i = 0; i < n && rc == FKX_OK; i++) {
    int differs = t[i].rows_differ;
#ifdef FKX_BUG_POLICY_IGNORES_NO_PK
    differs = 0; /* mutant: rows of a table without a primary key that changed are not noticed */
#endif
    if ((t[i].status == FKX_TABLE_NO_PK || t[i].status == FKX_TABLE_VIRTUAL) && differs)
      rc = refuse(x, refusal, "table '%s' %s: its rows changed, and a changeset cannot carry that", t[i].name, t[i].status == FKX_TABLE_NO_PK ? "has no PRIMARY KEY" : "is a virtual table or belongs to one");
  }
  tables_free(t, n);
  if (rc != FKX_OK) return rc;
#ifndef FKX_BUG_POLICY_IGNORES_TARGET_SCHEMA
  for (size_t i = 0; i < ntouched; i++) {
    char *ts = xinfo_sig(db, "main", touched[i], NULL), *os = xinfo_sig(db, "old", touched[i], NULL);
    if (!ts) rc = refuse(x, refusal, "table '%s' does not exist in '%s': the replay would silently skip its changes", touched[i], target_name);
    else if (!os || strcmp(ts, os)) rc = refuse(x, refusal, "table '%s' was changed in '%s' since the fork point (its columns or primary key differ): the replay would skip or misapply its changes", touched[i], target_name);
    free(ts); free(os);
    if (rc != FKX_OK) return rc;
  }
#else
  (void)touched; (void)ntouched; (void)target_name;
#endif
  return FKX_OK;
}

/* old_ref -> new_ref is the change to replay onto target. See fkx.h. take_undo: record an undo point at commit. */
static int replay_begin_ex(fkx *x, const char *old_ref, const char *new_ref, const char *target, const char *const *touched, size_t ntouched, int take_undo, fkx_replay **rout, sqlite3 **dbout, char **refusal) {
  uint32_t pid, oid, nid;
  Meta pm, om, nm;
  int rc;
  if (refusal) *refusal = NULL;
  if ((rc = fkx_i_resolve(x, target, &pid, &pm)) || (rc = fkx_i_resolve(x, old_ref, &oid, &om)) || (rc = fkx_i_resolve(x, new_ref, &nid, &nm))) return rc;
  if (pm.ro || (pm.kind != FKX_ROOT && pm.kind != FKX_FORK)) return fkx_i_fail(x, FKX_EREADONLY, "'%s' cannot be written", target);
  fkx_replay *r = calloc(1, sizeof *r);
  if (!r) return FKX_ENOMEM;
  r->x = x; r->pid = pid; r->pm = pm; r->take_undo = take_undo;
  char tref[24];
  fkx_i_ref(pm.uid, tref);
  if ((rc = fkx_open_db(x, tref, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, &r->db))) { free(r); return rc; }
  int srq;
  /* Foreign keys: the changeset carries every row change, including those a foreign key action made in the fork, so the actions must not run
   * a second time: they are off during the apply, and the whole database is checked for violations before the commit instead. */
  srq = sq_exec(r->db, "PRAGMA foreign_keys=OFF");
  if (srq == SQLITE_OK) { char *q = sqlite3_mprintf("ATTACH 'file:b%u?mode=ro' AS old", (unsigned)oid); srq = q ? sq_exec(r->db, q) : SQLITE_NOMEM; sqlite3_free(q); }
  if (srq == SQLITE_OK) { char *q = sqlite3_mprintf("ATTACH 'file:b%u?mode=ro' AS new", (unsigned)nid); srq = q ? sq_exec(r->db, q) : SQLITE_NOMEM; sqlite3_free(q); }
  if (srq == SQLITE_OK) srq = sq_exec(r->db, "BEGIN IMMEDIATE");
  if (srq != SQLITE_OK) {
    int busy = sqlite3_errcode(r->db) == SQLITE_BUSY || sqlite3_errcode(r->db) == SQLITE_LOCKED;
    rc = busy ? fkx_i_fail(x, FKX_EBUSY, "'%s' is being written by another connection", target) : fkx_i_fail(x, FKX_ESQLITE, "starting the replay on '%s' failed: %s", target, sqlite3_errmsg(r->db));
    sqlite3_close(r->db);
    free(r);
    return rc;
  }
  if ((rc = check_policy(x, r->db, touched, ntouched, pm.name[0] ? pm.name : target, refusal))) goto fail;
  /* the target's triggers on the touched tables: their effects are in the changeset, so they must not fire again */
  for (size_t i = 0; i < ntouched; i++) {
    char *q = sqlite3_mprintf("SELECT name, sql FROM main.sqlite_schema WHERE type='trigger' AND tbl_name='%q'", touched[i]);
    sqlite3_stmt *st = NULL;
    if (!q || sqlite3_prepare_v2(r->db, q, -1, &st, NULL) != SQLITE_OK) { sqlite3_free(q); rc = fkx_i_fail(x, FKX_ESQLITE, "reading triggers failed: %s", sqlite3_errmsg(r->db)); goto fail; }
    sqlite3_free(q);
    while (sqlite3_step(st) == SQLITE_ROW) {
      char *nm = strdup((const char *)sqlite3_column_text(st, 0)), *sq = strdup((const char *)sqlite3_column_text(st, 1));
      if (!nm || !sq) { free(nm); free(sq); sqlite3_finalize(st); rc = FKX_ENOMEM; goto fail; }
      char **g = realloc(r->trig, (r->ntrig + 1) * sizeof *g);
      if (!g) { free(nm); free(sq); sqlite3_finalize(st); rc = FKX_ENOMEM; goto fail; }
      r->trig = g;
      r->trig[r->ntrig++] = sq;
#ifndef FKX_BUG_REPLAY_KEEPS_TRIGGERS
      if (sq_execf(r->db, "DROP TRIGGER main.\"%w\"%s", nm, "") != SQLITE_OK) { rc = fkx_i_fail(x, FKX_ESQLITE, "dropping trigger %s failed: %s", nm, sqlite3_errmsg(r->db)); free(nm); sqlite3_finalize(st); goto fail; }
#endif
      free(nm);
    }
    sqlite3_finalize(st);
  }
  if (take_undo && (rc = fkx_i_undo_before(x, pid, &pm, &r->ub))) goto fail;
  if ((rc = fkx_i_track(x, r, replay_abort_cb))) goto fail;
  *rout = r;
  *dbout = r->db;
  return FKX_OK;
fail:
  sq_exec(r->db, "ROLLBACK");
  sqlite3_close(r->db);
  r->db = NULL;
  if (r->ub) (void)fkx_i_discard_uid(x, r->ub);
  replay_free(r);
  return rc;
}

int fkx_replay_begin(fkx *x, const char *old_ref, const char *new_ref, const char *target, const char *const *touched, size_t ntouched, fkx_replay **r, sqlite3 **db, char **refusal) {
  return replay_begin_ex(x, old_ref, new_ref, target, touched, ntouched, 1, r, db, refusal);
}

/* re-create the triggers, check the foreign keys, commit; fk_table gets the first table with a violation if that is what stopped it */
static int replay_commit_ex(fkx_replay *r, char **fk_table) {
  fkx *x = r->x;
  if (fk_table) *fk_table = NULL;
  for (size_t i = 0; i < r->ntrig; i++) {
#ifdef FKX_BUG_TRIGGERS_NOT_RECREATED
    break; /* mutant: the triggers the replay dropped are not put back */
#endif
    if (sq_exec(r->db, r->trig[i]) != SQLITE_OK) { int rc = fkx_i_fail(x, FKX_ESQLITE, "re-creating a trigger failed: %s", sqlite3_errmsg(r->db)); replay_abort_impl(r); return rc; }
  }
#ifndef FKX_BUG_REPLAY_NO_FK_CHECK
  {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(r->db, "PRAGMA main.foreign_key_check", -1, &st, NULL) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (fk_table) *fk_table = strdup(t ? (const char *)t : "?");
        int rc = fkx_i_fail(x, FKX_ECONFLICT, "the replay would leave a foreign key violation in table '%s': nothing was changed", t ? (const char *)t : "?");
        sqlite3_finalize(st);
        replay_abort_impl(r);
        return rc;
      }
      sqlite3_finalize(st);
    }
  }
#endif
  if (sq_exec(r->db, "COMMIT") != SQLITE_OK) {
    int busy = sqlite3_errcode(r->db) == SQLITE_BUSY;
    int rc = busy ? fkx_i_fail(x, FKX_EBUSY, "committing the replay failed: the database is busy") : fkx_i_fail(x, FKX_ESQLITE, "committing the replay failed: %s", sqlite3_errmsg(r->db));
    replay_abort_impl(r);
    return rc;
  }
  sqlite3_close(r->db);
  r->db = NULL;
  int rc = FKX_OK;
#ifndef FKX_BUG_REPLAY_NO_UNDO_RECORD
  if (r->take_undo) rc = fkx_i_undo_after(x, r->pid, &r->pm, r->ub, &r->recorded);
#endif
  r->ub = 0; /* listed (or not wanted): not garbage */
  fkx_i_untrack(x, r);
  replay_free(r);
  return rc;
}
int fkx_replay_commit(fkx_replay *r) { return replay_commit_ex(r, NULL); }

/* ---------- applying a changeset, and promote / undo by replay ---------- */
typedef struct {
  fkx_conflict *c;
  size_t n, cap;
  int collect_all;
} ConfCtx;

static char *key_text(sqlite3_changeset_iter *it, int op) {
  const char *tab;
  int nc, o, ind;
  unsigned char *pk = NULL;
  int npk = 0;
  sqlite3changeset_op(it, &tab, &nc, &o, &ind);
  if (sqlite3changeset_pk(it, &pk, &npk) != SQLITE_OK) return strdup("?");
  sqlite3_str *s = sqlite3_str_new(NULL);
  int first = 1;
  for (int i = 0; i < npk; i++) {
    if (!pk[i]) continue;
    sqlite3_value *v = NULL;
    if (op == SQLITE_INSERT) sqlite3changeset_new(it, i, &v); else sqlite3changeset_old(it, i, &v);
    const unsigned char *t = v ? sqlite3_value_text(v) : NULL;
    sqlite3_str_appendf(s, "%s%d=%s", first ? "" : ",", i, t ? (const char *)t : "NULL");
    first = 0;
  }
  char *z = sqlite3_str_finish(s);
  char *out = strdup(z ? z : "?");
  sqlite3_free(z);
  return out;
}

static int conflict_cb(void *p, int type, sqlite3_changeset_iter *it) {
  ConfCtx *c = p;
  const char *tab = "?";
  int nc, op = SQLITE_UPDATE, ind;
  sqlite3changeset_op(it, &tab, &nc, &op, &ind);
  if (c->n == c->cap) {
    size_t cap = c->cap ? c->cap * 2 : 8;
    fkx_conflict *g = realloc(c->c, cap * sizeof *g);
    if (!g) return SQLITE_CHANGESET_ABORT;
    c->c = g; c->cap = cap;
  }
  fkx_conflict *k = &c->c[c->n++];
  k->table = strdup(tab); k->kind = type; k->op = op; k->key = key_text(it, op);
#ifdef FKX_BUG_CONFLICT_OMITS
  return SQLITE_CHANGESET_OMIT; /* mutant: a conflicting change is skipped and the rest applied */
#endif
  return c->collect_all ? SQLITE_CHANGESET_OMIT : SQLITE_CHANGESET_ABORT;
}

/* the names of the tables a changeset touches (unique) */
static int touched_tables(int n, void *cs, char ***out, size_t *nout) {
  sqlite3_changeset_iter *it = NULL;
  char **v = NULL;
  size_t nv = 0;
  *out = NULL; *nout = 0;
  if (sqlite3changeset_start(&it, n, cs) != SQLITE_OK) return FKX_ESQLITE;
  while (sqlite3changeset_next(it) == SQLITE_ROW) {
    const char *tab;
    int nc, op, ind;
    sqlite3changeset_op(it, &tab, &nc, &op, &ind);
    size_t i;
    for (i = 0; i < nv; i++) if (!strcmp(v[i], tab)) break;
    if (i < nv) continue;
    char **g = realloc(v, (nv + 1) * sizeof *g);
    if (!g) { sqlite3changeset_finalize(it); for (size_t j = 0; j < nv; j++) free(v[j]); free(v); return FKX_ENOMEM; }
    v = g;
    v[nv++] = strdup(tab);
  }
  sqlite3changeset_finalize(it);
  *out = v; *nout = nv;
  return FKX_OK;
}
static void strs_free(char **v, size_t n) { for (size_t i = 0; i < n; i++) free(v[i]); free(v); }

/* diff(against -> ref), then replay it onto target. dry: change nothing and list every conflict. */
static int replay_change(fkx *x, const char *old_ref, const char *new_ref, const char *target, int dry, int take_undo, fkx_promote_result *out) {
  fkx_diff_result d;
  int rc = fkx_diff(x, new_ref, old_ref, &d);
  if (rc) return rc;
  char **touched = NULL;
  size_t nt = 0;
  if ((rc = touched_tables(d.nchangeset, d.changeset, &touched, &nt))) { fkx_diff_result_free(&d); return rc; }
  fkx_replay *r = NULL;
  sqlite3 *db = NULL;
  char *refusal = NULL;
  rc = replay_begin_ex(x, old_ref, new_ref, target, (const char *const *)touched, nt, dry ? 0 : take_undo, &r, &db, &refusal);
  if (rc) { out->refusal = refusal; strs_free(touched, nt); fkx_diff_result_free(&d); return rc; }
  ConfCtx cc;
  memset(&cc, 0, sizeof cc);
  cc.collect_all = dry;
  int arc = sqlite3changeset_apply_v2(db, d.nchangeset, d.changeset, NULL, conflict_cb, &cc, NULL, NULL, SQLITE_CHANGESETAPPLY_NOSAVEPOINT);
  strs_free(touched, nt);
  fkx_diff_result_free(&d);
  if (cc.n) {
    out->conflicts = cc.c; out->nconflicts = cc.n;
    rc = fkx_i_fail(x, FKX_ECONFLICT, "conflict in table '%s' (%s), key %s: nothing was changed", cc.c[0].table, cc.c[0].kind == SQLITE_CHANGESET_DATA ? "the row was changed in the target" : cc.c[0].kind == SQLITE_CHANGESET_NOTFOUND ? "the row is not in the target" : cc.c[0].kind == SQLITE_CHANGESET_CONFLICT ? "the key exists in the target" : cc.c[0].kind == SQLITE_CHANGESET_CONSTRAINT ? "a constraint failed" : "a foreign key failed", cc.c[0].key);
    fkx_replay_abort(r);
    return rc;
  }
  free(cc.c);
  if (arc != SQLITE_OK) { rc = fkx_i_fail(x, FKX_ESQLITE, "applying the changeset failed (SQLite code %d)", arc); fkx_replay_abort(r); return rc; }
  if (dry) {
    /* the foreign keys too, then roll everything back */
    sqlite3_stmt *st = NULL;
    int bad = 0;
    char *tn = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA main.foreign_key_check", -1, &st, NULL) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) { bad = 1; const unsigned char *t = sqlite3_column_text(st, 0); tn = strdup(t ? (const char *)t : "?"); }
      sqlite3_finalize(st);
    }
    fkx_replay_abort(r);
    if (bad) {
      out->conflicts = calloc(1, sizeof *out->conflicts);
      if (out->conflicts) { out->nconflicts = 1; out->conflicts[0].table = tn; out->conflicts[0].kind = SQLITE_CHANGESET_FOREIGN_KEY; out->conflicts[0].op = SQLITE_UPDATE; out->conflicts[0].key = strdup("(foreign key)"); }
      else free(tn);
      return fkx_i_fail(x, FKX_ECONFLICT, "the replay would leave a foreign key violation in table '%s'", out->conflicts ? out->conflicts[0].table : "?");
    }
    return FKX_OK;
  }
  char *fkt = NULL;
  rc = replay_commit_ex(r, &fkt); /* r is consumed */
  if (rc == FKX_ECONFLICT && fkt) {
    out->conflicts = calloc(1, sizeof *out->conflicts);
    if (out->conflicts) { out->nconflicts = 1; out->conflicts[0].table = fkt; out->conflicts[0].kind = SQLITE_CHANGESET_FOREIGN_KEY; out->conflicts[0].op = SQLITE_UPDATE; out->conflicts[0].key = strdup("(foreign key)"); }
    else free(fkt);
  } else free(fkt);
  return rc;
}

int fkx_i_replay_promote(fkx *x, uint32_t fid, const Meta *fm, uint32_t pid, const Meta *pm, uint32_t bid, fkx_promote_result *out) {
  (void)fid; (void)pid; (void)bid;
  char fref[24], bref[24], tref[24];
  fkx_i_ref(fm->uid, fref); fkx_i_ref(fm->base, bref); fkx_i_ref(pm->uid, tref);
  out->method = FKX_METHOD_REPLAY;
  int rc = replay_change(x, bref, fref, tref, out->dry_run, 1, out);
  if (rc == FKX_OK && !out->dry_run) {
    /* the undo point was recorded by the commit when retention is not 0 */
    out->undo_recorded = fkx_i_undo_keep(x) != 0;
  }
  return rc;
}

int fkx_i_replay_undo(fkx *x, uint32_t pid, const Meta *pm, fkx_promote_result *out) {
  (void)pid;
  char bref[24], aref[24], tref[24];
#ifdef FKX_BUG_UNDO_REPLAY_WRONG_DIRECTION
  fkx_i_ref(pm->undo[0][1], bref); fkx_i_ref(pm->undo[0][0], aref); /* mutant: the promote is replayed again instead of inverted */
#else
  fkx_i_ref(pm->undo[0][0], bref); fkx_i_ref(pm->undo[0][1], aref);
#endif
  fkx_i_ref(pm->uid, tref);
  out->method = FKX_METHOD_REPLAY;
  /* after -> before, onto the target as it is now: the work done on the target since is kept */
  return replay_change(x, aref, bref, tref, out->dry_run, 0, out);
}
