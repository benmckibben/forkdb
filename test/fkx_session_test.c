/* fkx session driver tests: diff, promote and undo by replay, the schema policy, triggers, foreign keys, the stepwise primitives. Real files, SQLite
 * through the VFS, built with the session extension (see scripts/test-fkx.sh). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../fkx/fkx_internal.h"
#include "../vfs/fkvfs.h"
#include "sqlite3session.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char dir[] = "/tmp/fkxsesXXXXXX";
static char path[300];
static const char *P(const char *n) { snprintf(path, sizeof path, "%s/%s", dir, n); return path; }

static fkx *open_x(const char *file, unsigned keep) {
  fkx_options o;
  fkx_options_default(&o);
  o.prealloc_chunk = 0;
  o.undo_keep = keep;
  fkx *x = NULL;
  int rc = fkx_open(P(file), &o, &x);
  if (rc) { printf("  fkx_open(%s) = %d\n", file, rc); return NULL; }
  return x;
}
static int sql(fkx *x, const char *ref, const char *q) {
  sqlite3 *db = NULL;
  if (fkx_open_db(x, ref, SQLITE_OPEN_READWRITE, &db)) return -1;
  char *err = NULL;
  int rc = sqlite3_exec(db, q, 0, 0, &err);
  if (rc) printf("  sql on %s: %s (%s)\n", ref, err ? err : "?", q);
  sqlite3_free(err);
  sqlite3_close(db);
  return rc;
}
static char qbuf[2][2048];
static int qi;
/* the rows of a query as one text: "a|b;a|b;" */
static const char *rows(fkx *x, const char *ref, const char *q) {
  char *out = qbuf[qi ^= 1];
  out[0] = 0;
  sqlite3 *db = NULL;
  if (fkx_open_db(x, ref, SQLITE_OPEN_READONLY, &db)) { snprintf(out, sizeof qbuf[0], "(cannot open %s)", ref); return out; }
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, q, -1, &st, 0) != SQLITE_OK) { snprintf(out, sizeof qbuf[0], "(error: %s)", sqlite3_errmsg(db)); sqlite3_close(db); return out; }
  size_t n = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    for (int i = 0; i < sqlite3_column_count(st); i++) {
      const unsigned char *t = sqlite3_column_text(st, i);
      n += (size_t)snprintf(out + n, sizeof qbuf[0] - n, "%s%s", i ? "|" : "", t ? (const char *)t : "NULL");
    }
    n += (size_t)snprintf(out + n, sizeof qbuf[0] - n, ";");
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  return out;
}
static int same(const char *a, const char *b) { return strcmp(a, b) == 0; }
static size_t nlist(fkx *x, int internal) { size_t n = 0; fkx_list(x, internal, NULL, 0, &n); return n; }
static int consistent(fkx *x) { char why[200]; int rc = fkx_check(x, why, sizeof why); if (rc) printf("  inconsistent: %s\n", why); return rc == 0; }
static uint64_t state_of(fkx *x, const char *ref) { fkx_info i; return fkx_info_get(x, ref, &i) ? 0 : i.state; }
static int nundo(fkx *x, const char *ref) { fkx_info i; return fkx_info_get(x, ref, &i) ? -1 : (int)i.n_undo; }
static uint64_t commits(fkx *x) { FkVfsStats s; fkvfs_stats(fkx_vfs(x), &s); return s.store_commits; }
static const fkx_table_diff *tab(const fkx_diff_result *d, const char *n) { for (size_t i = 0; i < d->ntables; i++) if (!strcmp(d->tables[i].name, n)) return &d->tables[i]; return NULL; }

static const char *SETUP =
  "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT);"
  "CREATE TABLE t2(a, b);"
  "CREATE TABLE t3(x, y, z, PRIMARY KEY(x, y)) WITHOUT ROWID;"
  "WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<10) INSERT INTO t1 SELECT i, 'v'||i FROM c;"
  "INSERT INTO t2 VALUES(1,'a'),(2,'b');"
  "INSERT INTO t3 VALUES(1,1,'p'),(2,2,'q');";

static void t_diff(void) {
  printf("S1 diff: counts, statuses, nothing left out\n");
  fkx *x = open_x("s1.fk", 8);
  if (!x) { CHECK(0, "open"); return; }
  CHECK(fkx_have_session() == 1, "this build has the session driver");
  sql(x, "main", SETUP);
  fkx_fork(x, "main", "f");
  sql(x, "f", "UPDATE t1 SET v='changed' WHERE id IN (2,4); INSERT INTO t1 VALUES(50,'new'); DELETE FROM t1 WHERE id=9; INSERT INTO t2 VALUES(3,'c'); CREATE TABLE t4(id INTEGER PRIMARY KEY, w); INSERT INTO t4 VALUES(1,'x')");
  fkx_diff_result d;
  CHECK(fkx_diff(x, "f", NULL, &d) == 0, "diff a fork against its base");
  const fkx_table_diff *t1 = tab(&d, "t1"), *t2 = tab(&d, "t2"), *t3 = tab(&d, "t3"), *t4 = tab(&d, "t4");
  CHECK(d.ntables == 4 && t1 && t2 && t3 && t4, "every table of either side is listed (%zu)", d.ntables);
  CHECK(t1 && t1->status == FKX_TABLE_COMPARED && t1->inserts == 1 && t1->updates == 2 && t1->deletes == 1, "t1: compared, 1 insert, 2 updates, 1 delete");
  CHECK(t3 && t3->status == FKX_TABLE_COMPARED && t3->inserts + t3->updates + t3->deletes == 0, "t3 (WITHOUT ROWID, composite key): compared, no change");
  CHECK(t2 && t2->status == FKX_TABLE_NO_PK && t2->rows_differ == 1, "t2 has no primary key: not diffable, and its rows differ (the library would have said nothing)");
  CHECK(t4 && t4->status == FKX_TABLE_ONLY_HERE, "t4 exists only in the fork");
  CHECK(d.schema_differs == 1 && d.nchangeset > 0 && d.changeset, "the schema differs (a table was created), and there is a changeset");
  fkx_diff_result_free(&d);
  fkx_fork(x, "main", "g");
  sql(x, "g", "UPDATE t1 SET v='g' WHERE id=1");
  CHECK(fkx_diff(x, "g", NULL, &d) == 0 && d.schema_differs == 0 && tab(&d, "t1")->updates == 1 && tab(&d, "t2")->status == FKX_TABLE_COMPARED + FKX_TABLE_NO_PK && tab(&d, "t2")->rows_differ == 0, "a fork that only changed rows: no schema change, and the no-key table is shown as unchanged");
  fkx_diff_result_free(&d);
  CHECK(fkx_diff(x, "g", "f", &d) == 0 && tab(&d, "t4") && tab(&d, "t4")->status == FKX_TABLE_ONLY_THERE && d.schema_differs == 1, "any two branches can be compared; a table only the other has is said so");
  fkx_diff_result_free(&d);
  CHECK(fkx_diff(x, "main", NULL, &d) == FKX_EINVAL && fkx_diff(x, "nope", NULL, &d) == FKX_ENOENT, "the root has no base to compare with; a branch that is not there");
  fkx_fork(x, "main", "h");
  sql(x, "h", "ALTER TABLE t1 ADD COLUMN w TEXT");
  CHECK(fkx_diff(x, "h", NULL, &d) == 0 && tab(&d, "t1")->status == FKX_TABLE_SCHEMA && d.schema_differs == 1, "a table whose columns changed is marked SCHEMA and not compared");
  fkx_diff_result_free(&d);
  CHECK(nlist(x, 1) == 7 && consistent(x), "a diff leaves nothing behind (main, three forks and their bases)");
  fkx_close(x);
}

static void t_replay(void) {
  printf("S2 promote by replay, undo by replay\n");
  fkx *x = open_x("s2.fk", 8);
  if (!x) { CHECK(0, "open"); return; }
  sql(x, "main", SETUP);
  fkx_fork(x, "main", "f");
  sql(x, "f", "UPDATE t1 SET v='F1' WHERE id=1; INSERT INTO t1 VALUES(100,'f100'); DELETE FROM t1 WHERE id=10");
  sql(x, "main", "UPDATE t1 SET v='M3' WHERE id=3; INSERT INTO t1 VALUES(200,'m200')");
  const char *before = "1|v1;2|v2;3|M3;4|v4;5|v5;6|v6;7|v7;8|v8;9|v9;10|v10;200|m200;";
  CHECK(same(rows(x, "main", "SELECT id, v FROM t1 ORDER BY id"), before), "main has moved on its own rows");
  uint64_t c0 = commits(x);
  fkx_promote_result r;
  fkx_promote_opts dry = {NULL, 1};
  CHECK(fkx_promote(x, "f", &dry, &r) == 0 && r.method == FKX_METHOD_REPLAY && same(rows(x, "main", "SELECT id, v FROM t1 ORDER BY id"), before) && nundo(x, "main") == 0 && nlist(x, 1) == 3 && commits(x) == c0, "a dry run says replay, and changes nothing (no commit, no snapshot)");
  fkx_promote_result_free(&r);
  CHECK(fkx_promote(x, "f", NULL, &r) == 0 && r.method == FKX_METHOD_REPLAY && r.undo_recorded, "promote: replay, undo point recorded");
  fkx_promote_result_free(&r);
  CHECK(commits(x) == c0 + 1, "a replay is exactly one store commit (%llu)", (unsigned long long)(commits(x) - c0));
  const char *after = "1|F1;2|v2;3|M3;4|v4;5|v5;6|v6;7|v7;8|v8;9|v9;100|f100;200|m200;";
  CHECK(same(rows(x, "main", "SELECT id, v FROM t1 ORDER BY id"), after), "main has the fork's work and its own: '%s'", rows(x, "main", "SELECT id, v FROM t1 ORDER BY id"));
  CHECK(nundo(x, "main") == 1 && nlist(x, 1) == 5 && consistent(x), "one undo point (two snapshots)");
  CHECK(same(rows(x, "f", "SELECT id, v FROM t1 WHERE id IN (1,100) ORDER BY id"), "1|F1;100|f100;"), "the fork is unchanged");
  /* main moves again, then undo: the later work is kept */
  sql(x, "main", "UPDATE t1 SET v='M4' WHERE id=4; INSERT INTO t1 VALUES(300,'m300')");
  CHECK(fkx_undo(x, "main", &r) == 0 && r.method == FKX_METHOD_REPLAY, "undo by replay (main changed since the promote)");
  fkx_promote_result_free(&r);
  CHECK(same(rows(x, "main", "SELECT id, v FROM t1 ORDER BY id"), "1|v1;2|v2;3|M3;4|M4;5|v5;6|v6;7|v7;8|v8;9|v9;10|v10;200|m200;300|m300;"), "the promote is undone (id 1 back, 100 gone, 10 back) and what main did before and after it is kept: '%s'", rows(x, "main", "SELECT id, v FROM t1 ORDER BY id"));
  CHECK(nundo(x, "main") == 0 && nlist(x, 1) == 3 && consistent(x), "the entry and its snapshots are gone");
  /* and an undo by replay where main is exactly where the promote left it takes the exact path */
  fkx_fork(x, "main", "g");
  sql(x, "g", "UPDATE t1 SET v='G' WHERE id=2");
  CHECK(fkx_promote(x, "g", NULL, &r) == 0 && r.method == FKX_METHOD_FAST_FORWARD, "(a promote that can fast-forward still does)");
  fkx_promote_result_free(&r);
  CHECK(fkx_undo(x, "main", &r) == 0 && r.method == FKX_METHOD_FAST_FORWARD, "and its undo is exact");
  fkx_promote_result_free(&r);
  /* connections open on the target: the replay path is used instead of refusing */
  fkx_fork(x, "main", "k");
  sql(x, "k", "UPDATE t1 SET v='K' WHERE id=5");
  sqlite3 *db = NULL;
  CHECK(fkx_open_db(x, "main", SQLITE_OPEN_READONLY, &db) == 0, "a connection on main");
  sqlite3_stmt *st = NULL;
  sqlite3_prepare_v2(db, "SELECT v FROM t1 WHERE id=5", -1, &st, 0);
  CHECK(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "v5"), "(it has read the row and has pages cached)");
  sqlite3_finalize(st);
  CHECK(fkx_promote(x, "k", NULL, &r) == 0 && r.method == FKX_METHOD_REPLAY && r.undo_recorded, "promote with a connection open on main: replayed, not refused");
  fkx_promote_result_free(&r);
  sqlite3_prepare_v2(db, "SELECT v FROM t1 WHERE id=5", -1, &st, 0);
  CHECK(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "K"), "and the open connection sees the promoted row (its cache was not stale)");
  sqlite3_finalize(st);
  fkx_fork(x, "main", "m");
  sql(x, "m", "CREATE TABLE extra(id INTEGER PRIMARY KEY)");
  CHECK(fkx_promote(x, "m", NULL, &r) == FKX_EBUSY && strstr(fkx_errmsg(x), "replay was not possible"), "a fork with DDL and a connection open on main: neither path is possible: busy, and it says why (%s)", fkx_errmsg(x));
  fkx_promote_result_free(&r);
  sqlite3_close(db);
  CHECK(fkx_promote(x, "m", NULL, &r) == 0 && r.method == FKX_METHOD_FAST_FORWARD && !strcmp(rows(x, "main", "SELECT name FROM sqlite_schema WHERE name='extra'"), "extra;"), "with the connection closed the fast-forward carries the DDL");
  fkx_promote_result_free(&r);
  CHECK(consistent(x), "consistent");
  fkx_close(x);
}

static void t_conflicts(void) {
  printf("S3 conflicts leave the target untouched\n");
  fkx *x = open_x("s3.fk", 8);
  if (!x) { CHECK(0, "open"); return; }
  sql(x, "main", SETUP);
  fkx_fork(x, "main", "f");
  sql(x, "f", "UPDATE t1 SET v='F' WHERE id IN (1,2); INSERT INTO t1 VALUES(50,'f50'); UPDATE t1 SET v='ok' WHERE id=8");
  sql(x, "main", "UPDATE t1 SET v='M' WHERE id IN (1,2); INSERT INTO t1 VALUES(50,'m50')");
  const char *snap = rows(x, "main", "SELECT id, v FROM t1 ORDER BY id");
  char keep[1024];
  snprintf(keep, sizeof keep, "%s", snap);
  uint64_t st0 = state_of(x, "main");
  size_t nl = nlist(x, 1);
  fkx_promote_result r;
  CHECK(fkx_promote(x, "f", NULL, &r) == FKX_ECONFLICT && r.nconflicts >= 1 && r.conflicts && !strcmp(r.conflicts[0].table, "t1"), "promote: a conflict, in t1");
  CHECK(r.nconflicts == 1, "the first one only (%zu)", r.nconflicts);
  fkx_promote_result_free(&r);
  CHECK(same(rows(x, "main", "SELECT id, v FROM t1 ORDER BY id"), keep) && state_of(x, "main") == st0 && nundo(x, "main") == 0 && nlist(x, 1) == nl && consistent(x), "nothing changed: not a row, not the state id, no undo point, no snapshot left behind");
  fkx_promote_opts dry = {NULL, 1};
  CHECK(fkx_promote(x, "f", &dry, &r) == FKX_ECONFLICT && r.nconflicts == 3, "a dry run lists every conflict (%zu: ids 1 and 2 changed on both sides, 50 inserted on both)", r.nconflicts);
  int kinds_ok = 0;
  for (size_t i = 0; i < r.nconflicts; i++) kinds_ok += r.conflicts[i].kind == SQLITE_CHANGESET_DATA || r.conflicts[i].kind == SQLITE_CHANGESET_CONFLICT;
  CHECK(kinds_ok == 3 && r.conflicts[0].key && r.conflicts[0].key[0], "each has a kind and a key");
  fkx_promote_result_free(&r);
  CHECK(same(rows(x, "main", "SELECT id, v FROM t1 ORDER BY id"), keep) && state_of(x, "main") == st0 && nlist(x, 1) == nl, "and changed nothing");
  fkx_close(x);
}

static void t_policy(void) {
  printf("S4 the schema policy: refusals change nothing\n");
  fkx *x = open_x("s4.fk", 8);
  if (!x) { CHECK(0, "open"); return; }
  sql(x, "main", SETUP);
  fkx_promote_result r;
  /* (a) the fork changed the schema, and main moved: no replay, no fast-forward */
  fkx_fork(x, "main", "ddl");
  sql(x, "ddl", "ALTER TABLE t1 ADD COLUMN w TEXT; UPDATE t1 SET v='D' WHERE id=1");
  sql(x, "main", "UPDATE t1 SET v='M' WHERE id=3");
  uint64_t st0 = state_of(x, "main");
  size_t nl = nlist(x, 1);
  CHECK(fkx_promote(x, "ddl", NULL, &r) == FKX_EREFUSED && r.refusal && strstr(r.refusal, "schema"), "a fork that changed the schema, main moved: refused, with the rule ('%s')", r.refusal ? r.refusal : "");
  fkx_promote_result_free(&r);
  CHECK(state_of(x, "main") == st0 && nlist(x, 1) == nl && nundo(x, "main") == 0 && consistent(x), "nothing changed");
  /* (b) the fork changed rows of a table without a primary key */
  fkx_fork(x, "main", "nopk");
  sql(x, "nopk", "INSERT INTO t2 VALUES(9,'z'); UPDATE t1 SET v='N' WHERE id=2");
  sql(x, "main", "UPDATE t3 SET z='moved' WHERE x=2"); /* main moves, so only a replay could promote it */
  st0 = state_of(x, "main");
  nl = nlist(x, 1);
  CHECK(fkx_promote(x, "nopk", NULL, &r) == FKX_EREFUSED && r.refusal && strstr(r.refusal, "t2") && strstr(r.refusal, "PRIMARY KEY"), "a fork that changed a table without a primary key: refused, names the table ('%s')", r.refusal ? r.refusal : "");
  fkx_promote_result_free(&r);
  CHECK(state_of(x, "main") == st0 && nlist(x, 1) == nl && consistent(x), "nothing changed");
  /* (c) main changed the definition of a table the fork touched */
  fkx_discard(x, "ddl");
  fkx_discard(x, "nopk");
  fkx_fork(x, "main", "touch");
  sql(x, "touch", "UPDATE t1 SET v='T' WHERE id=4");
  sql(x, "main", "ALTER TABLE t1 ADD COLUMN extra TEXT");
  st0 = state_of(x, "main");
  nl = nlist(x, 1);
  CHECK(fkx_promote(x, "touch", NULL, &r) == FKX_EREFUSED && r.refusal && strstr(r.refusal, "t1") && strstr(r.refusal, "since the fork point"), "main changed the columns of a table the fork touched: refused ('%s')", r.refusal ? r.refusal : "");
  fkx_promote_result_free(&r);
  CHECK(state_of(x, "main") == st0 && nlist(x, 1) == nl && consistent(x), "nothing changed");
  /* (d) a column added to a table the fork did NOT touch is fine */
  fkx_discard(x, "touch");
  fkx_fork(x, "main", "other");
  sql(x, "other", "UPDATE t3 SET z='Z' WHERE x=1");
  sql(x, "main", "ALTER TABLE t1 ADD COLUMN more TEXT");
  CHECK(fkx_promote(x, "other", NULL, &r) == 0 && r.method == FKX_METHOD_REPLAY && !strcmp(rows(x, "main", "SELECT z FROM t3 WHERE x=1"), "Z;"), "a schema change in a table the fork did not touch does not matter");
  fkx_promote_result_free(&r);
  CHECK(consistent(x), "consistent");
  fkx_close(x);
}

static void t_triggers_fk(void) {
  printf("S5 triggers and foreign keys\n");
  fkx *x = open_x("s5.fk", 8);
  if (!x) { CHECK(0, "open"); return; }
  sql(x, "main",
      "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT); CREATE TABLE audit(n INTEGER PRIMARY KEY, id INT); CREATE TABLE other(id INTEGER PRIMARY KEY, v TEXT);"
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN INSERT INTO audit(id) VALUES(new.id); END;"
      "CREATE TABLE par(id INTEGER PRIMARY KEY); CREATE TABLE chi(id INTEGER PRIMARY KEY, pid INT REFERENCES par(id) ON DELETE CASCADE);"
      "INSERT INTO par VALUES(1),(2),(5); INSERT INTO chi VALUES(10,1),(11,1),(12,2); INSERT INTO other VALUES(1,'o');");
  fkx_promote_result r;
  /* a trigger in the target fires for the fork's inserts in the fork already: the changeset carries its rows, so it must not fire again */
  fkx_fork(x, "main", "f");
  sql(x, "f", "INSERT INTO t VALUES(1,'a'),(2,'b')");
  sql(x, "main", "UPDATE other SET v='moved' WHERE id=1");
  CHECK(fkx_promote(x, "f", NULL, &r) == 0 && r.method == FKX_METHOD_REPLAY, "promote with a trigger on a touched table");
  fkx_promote_result_free(&r);
  CHECK(same(rows(x, "main", "SELECT id FROM audit ORDER BY n"), "1;2;"), "the audit rows are the fork's, not doubled: '%s'", rows(x, "main", "SELECT id FROM audit ORDER BY n"));
  CHECK(same(rows(x, "main", "SELECT name FROM sqlite_schema WHERE type='trigger'"), "tr;"), "the trigger is still there");
  sql(x, "main", "INSERT INTO t VALUES(3,'c')");
  CHECK(same(rows(x, "main", "SELECT id FROM audit ORDER BY n"), "1;2;3;"), "and still fires");
  /* a foreign key action in the fork: its row changes are in the changeset, so the replay must not run the action again */
  fkx_fork(x, "main", "g");
  {
    sqlite3 *db = NULL;
    fkx_open_db(x, "g", SQLITE_OPEN_READWRITE, &db);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON; DELETE FROM par WHERE id=1", 0, 0, 0);
    sqlite3_close(db);
  }
  CHECK(same(rows(x, "g", "SELECT id FROM chi ORDER BY id"), "12;"), "(the fork deleted parent 1 and the cascade deleted its children)");
  sql(x, "main", "UPDATE other SET v='moved2' WHERE id=1");
  CHECK(fkx_promote(x, "g", NULL, &r) == 0 && r.method == FKX_METHOD_REPLAY, "promote: the cascade is carried as the rows it changed, without conflict (%d)", 0);
  fkx_promote_result_free(&r);
  CHECK(same(rows(x, "main", "SELECT id FROM par ORDER BY id"), "2;5;") && same(rows(x, "main", "SELECT id FROM chi ORDER BY id"), "12;"), "main: parent 1 and its children are gone");
  /* a foreign key violation the replay would leave: the fork adds a child of parent 5, main deleted parent 5 meanwhile */
  fkx_fork(x, "main", "h");
  sql(x, "h", "INSERT INTO chi VALUES(20,5)");
  sql(x, "main", "DELETE FROM par WHERE id=5");
  uint64_t st0 = state_of(x, "main");
  size_t nl = nlist(x, 1);
  CHECK(fkx_promote(x, "h", NULL, &r) == FKX_ECONFLICT && r.nconflicts == 1 && r.conflicts[0].kind == SQLITE_CHANGESET_FOREIGN_KEY && !strcmp(r.conflicts[0].table, "chi"), "an orphan row: a foreign key conflict in chi");
  fkx_promote_result_free(&r);
  CHECK(state_of(x, "main") == st0 && nlist(x, 1) == nl && same(rows(x, "main", "SELECT id FROM chi ORDER BY id"), "12;") && consistent(x), "and nothing was changed");
  fkx_promote_opts dry = {NULL, 1};
  CHECK(fkx_promote(x, "h", &dry, &r) == FKX_ECONFLICT && r.nconflicts == 1, "the dry run sees it too");
  fkx_promote_result_free(&r);
  fkx_close(x);
}

static int conf_abort(void *c, int type, sqlite3_changeset_iter *it) { (void)c; (void)type; (void)it; return SQLITE_CHANGESET_ABORT; }

static void t_primitives(void) {
  printf("S6 the stepwise primitives\n");
  fkx *x = open_x("s6.fk", 8);
  if (!x) { CHECK(0, "open"); return; }
  sql(x, "main", SETUP);
  sql(x, "main", "CREATE TABLE audit6(n INTEGER PRIMARY KEY, id INT); CREATE TRIGGER trg AFTER UPDATE ON t1 BEGIN INSERT INTO audit6(id) VALUES(new.id); END;");
  fkx_fork(x, "main", "f");
  sql(x, "main", "UPDATE t3 SET z='moved' WHERE x=1");
  {
    sqlite3 *db = NULL;
    fkx_open_db(x, "f", SQLITE_OPEN_READWRITE, &db);
    sqlite3_exec(db, "DROP TRIGGER trg", 0, 0, 0);
    sqlite3_close(db);
  }
  sql(x, "f", "CREATE TRIGGER trg AFTER UPDATE ON t1 BEGIN SELECT 1; END; UPDATE t1 SET v='P' WHERE id=2");
  fkx_info fi;
  fkx_info_get(x, "f", &fi);
  char fref[24], bref[24];
  fkx_i_ref(fi.uid, fref);
  fkx_i_ref(fi.base_uid, bref);
  /* the host's own diff on the handle */
  fkx_diff_handle *dh = NULL;
  sqlite3 *ddb = NULL;
  const fkx_table_diff *tabs = NULL;
  size_t nt = 0;
  int sd = 0;
  CHECK(fkx_diff_open(x, "f", NULL, &dh, &ddb, &tabs, &nt, &sd) == 0 && nt == 4 && ddb, "diff_open: a read connection with the other branch attached as 'other', and the table list");
  int sess_ok = 1, n = 0;
  void *cs = NULL;
  sqlite3_session *ss = NULL;
  sess_ok &= sqlite3session_create(ddb, "main", &ss) == SQLITE_OK;
  for (size_t i = 0; i < nt; i++) if (tabs[i].status == FKX_TABLE_COMPARED) sess_ok &= sqlite3session_attach(ss, tabs[i].name) == SQLITE_OK && sqlite3session_diff(ss, "other", tabs[i].name, NULL) == SQLITE_OK;
  sess_ok &= sqlite3session_changeset(ss, &n, &cs) == SQLITE_OK;
  sqlite3session_delete(ss);
  CHECK(sess_ok && n > 0, "the host runs its own session diff on it");
  sqlite3_free(cs);
  fkx_diff_close(dh);
  /* replay by hand */
  const char *touched[] = {"t1"};
  fkx_replay *rp = NULL;
  sqlite3 *rdb = NULL;
  char *refusal = NULL;
  size_t nl = nlist(x, 1);
  CHECK(fkx_replay_begin(x, bref, fref, "main", touched, 1, &rp, &rdb, &refusal) == FKX_EREFUSED && refusal && strstr(refusal, "schema") && !rp, "replay_begin: the fork changed a trigger (a schema change): refused, no handle");
  free(refusal);
  fkx_discard(x, "f");
  fkx_fork(x, "main", "f");
  sql(x, "f", "UPDATE t1 SET v='P' WHERE id=2");
  sql(x, "main", "UPDATE t3 SET z='moved2' WHERE x=1");
  fkx_info_get(x, "f", &fi);
  fkx_i_ref(fi.uid, fref);
  fkx_i_ref(fi.base_uid, bref);
  fkx_diff_result d;
  CHECK(fkx_diff(x, "f", NULL, &d) == 0, "diff");
  nl = nlist(x, 1);
  uint64_t st0 = state_of(x, "main");
  CHECK(fkx_replay_begin(x, bref, fref, "main", touched, 1, &rp, &rdb, &refusal) == 0 && rp && rdb, "replay_begin on a fork with only row changes");
  CHECK(rdb && sqlite3changeset_apply_v2(rdb, d.nchangeset, d.changeset, NULL, conf_abort, NULL, NULL, NULL, SQLITE_CHANGESETAPPLY_NOSAVEPOINT) == SQLITE_OK, "the host applies its changeset without a savepoint of its own");
  if (rp) fkx_replay_abort(rp);
  CHECK(state_of(x, "main") == st0 && nlist(x, 1) == nl && same(rows(x, "main", "SELECT name FROM sqlite_schema WHERE type='trigger'"), "trg;") && same(rows(x, "main", "SELECT v FROM t1 WHERE id=2"), "v2;") && consistent(x), "replay_abort: nothing changed, the trigger is back, no snapshot is left");
  CHECK(fkx_replay_begin(x, bref, fref, "main", touched, 1, &rp, &rdb, &refusal) == 0 && sqlite3changeset_apply_v2(rdb, d.nchangeset, d.changeset, NULL, conf_abort, NULL, NULL, NULL, SQLITE_CHANGESETAPPLY_NOSAVEPOINT) == SQLITE_OK, "again");
  CHECK(rp && fkx_replay_commit(rp) == 0, "replay_commit");
  CHECK(same(rows(x, "main", "SELECT v FROM t1 WHERE id=2"), "P;") && same(rows(x, "main", "SELECT name FROM sqlite_schema WHERE type='trigger'"), "trg;") && nundo(x, "main") == 1 && same(rows(x, "main", "SELECT id FROM audit6 ORDER BY n"), "2;"), "the change is in, the trigger did not fire a second time (one audit row, the fork's), the trigger is back, an undo point was recorded: '%s'", rows(x, "main", "SELECT id FROM audit6 ORDER BY n"));
  fkx_diff_result_free(&d);
  /* a forgotten handle is rolled back when the repository is closed */
  fkx_fork(x, "main", "g");
  sql(x, "g", "UPDATE t1 SET v='G' WHERE id=3");
  fkx_info_get(x, "g", &fi);
  fkx_i_ref(fi.uid, fref);
  fkx_i_ref(fi.base_uid, bref);
  fkx_diff(x, "g", NULL, &d);
  st0 = state_of(x, "main");
  CHECK(fkx_replay_begin(x, bref, fref, "main", touched, 1, &rp, &rdb, &refusal) == 0 && sqlite3changeset_apply_v2(rdb, d.nchangeset, d.changeset, NULL, conf_abort, NULL, NULL, NULL, SQLITE_CHANGESETAPPLY_NOSAVEPOINT) == SQLITE_OK, "a replay started and applied, and then forgotten");
  fkx_diff_result_free(&d);
  fkx_diff_handle *dh2 = NULL;
  fkx_diff_open(x, "g", NULL, &dh2, NULL, NULL, NULL, NULL);
  fkx_close(x);
  x = open_x("s6.fk", 8);
  CHECK(x && state_of(x, "main") == st0 && same(rows(x, "main", "SELECT v FROM t1 WHERE id=3"), "v3;") && same(rows(x, "main", "SELECT name FROM sqlite_schema WHERE type='trigger'"), "trg;") && consistent(x), "after close and reopen: main is as it was, with its trigger");
  if (x) {
    fkx_gc_report g;
    fkx_gc(x, &g);
    CHECK(nlist(x, 1) == 7 && nundo(x, "main") == 1, "and nothing is left over: main, f and g with their bases, and the one undo point (%zu)", nlist(x, 1));
    fkx_close(x);
  }
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (!mkdtemp(dir)) return 2;
  t_diff();
  t_replay();
  t_conflicts();
  t_policy();
  t_triggers_fk();
  t_primitives();
  char cmd[400];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  if (system(cmd)) {}
  printf("%d checks, TOTAL FAILS=%d\n", checks, fails);
  return fails ? 1 : 0;
}
