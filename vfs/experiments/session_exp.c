/* Experiments behind docs/SESSION-DESIGN.md: what SQLite's session extension does on forkdb branches (through the real VFS and a real
 * file store). Not part of the gate; run with vfs/experiments/session-experiments.sh, output in vfs/results/session-experiments.txt.
 * Each function is one experiment (X1 to X7) and prints what it observed. Throwaway code: it checks nothing, it reports. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef FK_EXP_DIR
#define FK_EXP_DIR "/tmp/forkdb-session-exp/"
#endif
#include "sqlite3.h"
#include "sqlite3session.h"
#include "fkvfs.h"
#include "fkstore.h"

static fk_io io; static fk_store *S; static FkVfs *V;
static int nlog;
static void logcb(void *a, int rc, const char *m) { (void)a; printf("    [sqlite log %d] %s\n", rc, m); nlog++; }
static void die(const char *m) { printf("FATAL %s\n", m); exit(1); }
static void setup(const char *path) {
  remove(path);
  if (fk_file_open(path, &io) != 0) die("file_open");
  if (fk_open(&io, &S) != FK_OK) die("fk_open");
  if (fkvfs_register(S, "forkdb", &V) != SQLITE_OK) die("register");
}
static sqlite3 *openb(uint32_t b) {
  char n[16]; fkvfs_branch_name(b, n, sizeof n);
  sqlite3 *db;
  int rc = sqlite3_open_v2(n, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "forkdb");
  if (rc) { printf("open %s: %s\n", n, sqlite3_errmsg(db)); exit(1); }
  return db;
}
static int ex(sqlite3 *db, const char *sql) {
  char *err = 0; int rc = sqlite3_exec(db, sql, 0, 0, &err);
  if (rc) { printf("    exec rc=%d (%s): %s   <- %.60s\n", rc, sqlite3_errstr(rc), err ? err : "", sql); sqlite3_free(err); }
  return rc;
}
static long long q1(sqlite3 *db, const char *sql) {
  sqlite3_stmt *st; long long v = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st); return v;
}
static char qbuf[4096];
static const char *qs(sqlite3 *db, const char *sql) {
  sqlite3_stmt *st; qbuf[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
    const char *t = (const char *)sqlite3_column_text(st, 0); snprintf(qbuf, sizeof qbuf, "%s", t ? t : "NULL"); }
  sqlite3_finalize(st); return qbuf;
}
static uint32_t fork_of(uint32_t src) { uint32_t d = 0; if (fk_fork(S, src, &d) != FK_OK) die("fork"); return d; }
static const char *opn(int op) { return op == SQLITE_INSERT ? "INSERT" : op == SQLITE_DELETE ? "DELETE" : "UPDATE"; }
static void dump_cs(int n, void *p, int maxshow) {
  sqlite3_changeset_iter *it; int ni = 0, nu = 0, nd = 0, shown = 0;
  if (sqlite3changeset_start(&it, n, p) != SQLITE_OK) { printf("    (bad changeset)\n"); return; }
  while (sqlite3changeset_next(it) == SQLITE_ROW) {
    const char *tab; int nc, op, ind; sqlite3changeset_op(it, &tab, &nc, &op, &ind);
    if (op == SQLITE_INSERT) ni++; else if (op == SQLITE_DELETE) nd++; else nu++;
    if (shown++ < maxshow) {
      printf("    %s %s", opn(op), tab);
      for (int i = 0; i < nc; i++) {
        sqlite3_value *o = 0, *nw = 0;
        if (op != SQLITE_INSERT) sqlite3changeset_old(it, i, &o);
        if (op != SQLITE_DELETE) sqlite3changeset_new(it, i, &nw);
        printf(" [%s%s%s]", o ? (const char *)sqlite3_value_text(o) : "", (o && nw) ? "->" : "", nw ? (const char *)sqlite3_value_text(nw) : "");
      }
      printf("\n");
    }
  }
  sqlite3changeset_finalize(it);
  printf("    total: %d inserts, %d updates, %d deletes, %d bytes\n", ni, nu, nd, n);
}
static int conf_n[8]; static int conf_ret = SQLITE_CHANGESET_ABORT;
static int conflict_cb(void *a, int type, sqlite3_changeset_iter *it) {
  (void)a; (void)it; if (type >= 1 && type <= 5) conf_n[type]++;
  printf("    conflict callback: %s\n", type == 1 ? "DATA" : type == 2 ? "NOTFOUND" : type == 3 ? "CONFLICT" : type == 4 ? "CONSTRAINT" : "FOREIGN_KEY");
  return conf_ret;
}
static int apply(sqlite3 *db, int n, void *p, const char *label) {
  memset(conf_n, 0, sizeof conf_n); nlog = 0;
  int rc = sqlite3changeset_apply(db, n, p, 0, conflict_cb, 0);
  printf("  apply[%s]: rc=%d (%s), conflicts DATA=%d NOTFOUND=%d CONFLICT=%d CONSTRAINT=%d FK=%d, log msgs=%d\n", label, rc, sqlite3_errstr(rc), conf_n[1], conf_n[2], conf_n[3], conf_n[4], conf_n[5], nlog);
  return rc;
}
static void fillt1(sqlite3 *db, int n) {
  ex(db, "BEGIN");
  sqlite3_stmt *st; sqlite3_prepare_v2(db, "INSERT INTO t1 VALUES(?, 'v'||?)", -1, &st, 0);
  for (int i = 1; i <= n; i++) { sqlite3_bind_int(st, 1, i); sqlite3_bind_int(st, 2, i); sqlite3_step(st); sqlite3_reset(st); }
  sqlite3_finalize(st); ex(db, "COMMIT");
}

/* ---------- X1: diff through ATTACH ---------- */
static void x1(void) {
  printf("== X1: diff of a fork against its parent through ATTACH + sqlite3session_diff\n");
  setup(FK_EXP_DIR "x1.fk");
  sqlite3 *d0 = openb(0);
  ex(d0, "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT); CREATE TABLE t2(a, b); CREATE TABLE t3(x, y, z, PRIMARY KEY(x, y)) WITHOUT ROWID;"
         "CREATE TABLE t5(id TEXT PRIMARY KEY, n INT);");
  fillt1(d0, 1000);
  ex(d0, "INSERT INTO t2 VALUES(1,'a'),(2,'b'); INSERT INTO t3 VALUES(1,1,'p'),(2,2,'q'); INSERT INTO t5 VALUES('k1',1),('k2',2)");
  sqlite3_close(d0);
  uint32_t b1 = fork_of(0);
  sqlite3 *d1 = openb(b1);
  ex(d1, "UPDATE t1 SET v='changed' WHERE id%100=0; INSERT INTO t1 VALUES(5000,'new1'),(5001,'new2'); DELETE FROM t1 WHERE id IN (7,8);"
         "INSERT INTO t2 VALUES(3,'c'); UPDATE t3 SET z='pp' WHERE x=1; DELETE FROM t5 WHERE id='k2'; CREATE TABLE t4(id INTEGER PRIMARY KEY, w);"
         "INSERT INTO t4 VALUES(1,'x')");
  ex(d1, "ATTACH 'b0' AS base");
  sqlite3_session *ss; sqlite3session_create(d1, "main", &ss);
  const char *tabs[] = {"t1", "t2", "t3", "t4", "t5"};
  for (int i = 0; i < 5; i++) {
    int rc = sqlite3session_attach(ss, tabs[i]);
    char *err = 0; rc = sqlite3session_diff(ss, "base", tabs[i], &err);
    printf("  diff %s: rc=%d (%s) %s\n", tabs[i], rc, sqlite3_errstr(rc), err ? err : ""); sqlite3_free(err);
  }
  int n = 0; void *p = 0; sqlite3session_changeset(ss, &n, &p);
  dump_cs(n, p, 40);
  sqlite3_free(p); sqlite3session_delete(ss);
  printf("  (t2 has no PRIMARY KEY, t4 does not exist in base)\n");
  sqlite3_close(d1);
  fk_close(S); fk_file_close(&io);
}

/* ---------- X2/X3: ATTACH write atomicity and read consistency ---------- */
static void x2(void) {
  printf("== X2: a write transaction across two attached branches\n");
  setup(FK_EXP_DIR "x2.fk");
  sqlite3 *d0 = openb(0);
  ex(d0, "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT)"); fillt1(d0, 10); sqlite3_close(d0);
  uint32_t b1 = fork_of(0), b2 = fork_of(0);
  sqlite3 *d1 = openb(b1);
  char sql[64]; snprintf(sql, sizeof sql, "ATTACH 'b%u' AS o", b2); ex(d1, sql);
  FkVfsStats a, b; fkvfs_stats(V, &a);
  ex(d1, "BEGIN; INSERT INTO main.t1 VALUES(9001,'x'); INSERT INTO o.t1 VALUES(9002,'y');");
  int rc = ex(d1, "COMMIT");
  fkvfs_stats(V, &b);
  printf("  commit rc=%d; journal_opens +%llu, sync_publish +%llu, store_commits +%llu\n", rc,
         (unsigned long long)(b.journal_opens - a.journal_opens), (unsigned long long)(b.sync_publish - a.sync_publish), (unsigned long long)(b.store_commits - a.store_commits));
  printf("  rows: b%u has 9001=%lld, b%u has 9002=%lld\n", b1, q1(d1, "SELECT count(*) FROM main.t1 WHERE id=9001"), b2, q1(d1, "SELECT count(*) FROM o.t1 WHERE id=9002"));
  sqlite3_close(d1);
  printf("== X3: two-branch read, then a writer on one of them\n");
  sqlite3 *r = openb(b1); snprintf(sql, sizeof sql, "ATTACH 'b%u' AS o", b2); ex(r, sql);
  ex(r, "BEGIN");
  long long c1 = q1(r, "SELECT count(*) FROM main.t1"), c2 = q1(r, "SELECT count(*) FROM o.t1");
  sqlite3 *w = openb(b2);
  int wrc = sqlite3_exec(w, "INSERT INTO t1 VALUES(9100,'late')", 0, 0, 0);
  printf("  reader sees %lld and %lld rows; writer on the second branch while the reader's txn is open: rc=%d (%s)\n", c1, c2, wrc, sqlite3_errstr(wrc));
  ex(r, "COMMIT");
  wrc = sqlite3_exec(w, "INSERT INTO t1 VALUES(9100,'late')", 0, 0, 0);
  printf("  after the reader commits: rc=%d\n", wrc);
  sqlite3_close(w); sqlite3_close(r);
  fk_close(S); fk_file_close(&io);
}

/* ---------- X4: schema drift on apply ---------- */
static void x4(void) {
  printf("== X4: changeset_apply against a parent whose schema moved\n");
  setup(FK_EXP_DIR "x4.fk");
  sqlite3_config(SQLITE_CONFIG_LOG, logcb, 0);
  sqlite3 *d0 = openb(0);
  ex(d0, "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT)"); fillt1(d0, 200); sqlite3_close(d0);
  uint32_t f = fork_of(0);
  uint32_t tgt[8]; for (int i = 0; i < 8; i++) tgt[i] = fork_of(0);
  sqlite3 *df = openb(f);
  sqlite3_session *ss; sqlite3session_create(df, "main", &ss); sqlite3session_attach(ss, "t1");
  ex(df, "UPDATE t1 SET v='changed' WHERE id%50=0; INSERT INTO t1 VALUES(900,'new'); DELETE FROM t1 WHERE id=7");
  int n = 0; void *p = 0; sqlite3session_changeset(ss, &n, &p);
  printf("  changeset from the fork:\n"); dump_cs(n, p, 3);
  const char *name[] = {"same schema", "column added at the end", "table dropped", "column renamed (v -> vv)", "primary key changed to (id, v)",
                        "column type changed (TEXT -> INTEGER)", "NOT NULL + CHECK added", "a column inserted before v"};
  const char *ddl[] = {"SELECT 1", "ALTER TABLE t1 ADD COLUMN w TEXT", "DROP TABLE t1", "ALTER TABLE t1 RENAME COLUMN v TO vv",
    "CREATE TABLE t1n(id INTEGER, v TEXT, PRIMARY KEY(id, v)); INSERT INTO t1n SELECT id,v FROM t1; DROP TABLE t1; ALTER TABLE t1n RENAME TO t1",
    "CREATE TABLE t1n(id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t1n SELECT id,length(v) FROM t1; DROP TABLE t1; ALTER TABLE t1n RENAME TO t1",
    "CREATE TABLE t1n(id INTEGER PRIMARY KEY, v TEXT NOT NULL CHECK(length(v)<8)); INSERT INTO t1n SELECT id,v FROM t1; DROP TABLE t1; ALTER TABLE t1n RENAME TO t1",
    "CREATE TABLE t1n(id INTEGER PRIMARY KEY, extra TEXT DEFAULT 'e', v TEXT); INSERT INTO t1n(id,v) SELECT id,v FROM t1; DROP TABLE t1; ALTER TABLE t1n RENAME TO t1"};
  for (int i = 0; i < 8; i++) {
    sqlite3 *dt = openb(tgt[i]);
    ex(dt, ddl[i]);
    apply(dt, n, p, name[i]);
    if (q1(dt, "SELECT count(*) FROM sqlite_schema WHERE name='t1'") == 1) {
      printf("    after: rows=%lld, id=900 present=%lld, id=7 present=%lld\n", q1(dt, "SELECT count(*) FROM t1"), q1(dt, "SELECT count(*) FROM t1 WHERE id=900"), q1(dt, "SELECT count(*) FROM t1 WHERE id=7"));
    }
    sqlite3_close(dt);
  }
  sqlite3_free(p); sqlite3session_delete(ss); sqlite3_close(df);
  printf("  does a session record DDL? creating a table in a fork with a session attached to all tables:\n");
  uint32_t g = fork_of(0); sqlite3 *dg = openb(g);
  sqlite3_session *s2; sqlite3session_create(dg, "main", &s2); sqlite3session_attach(s2, 0);
  ex(dg, "CREATE TABLE t9(id INTEGER PRIMARY KEY, w); INSERT INTO t9 VALUES(1,'x'); ALTER TABLE t1 ADD COLUMN z");
  sqlite3session_changeset(s2, &n, &p); dump_cs(n, p, 5); sqlite3_free(p); sqlite3session_delete(s2); sqlite3_close(dg);
  fk_close(S); fk_file_close(&io);
}

/* ---------- X5: triggers and foreign keys ---------- */
static void x5(void) {
  printf("== X5: triggers and foreign keys under a session and under apply\n");
  setup(FK_EXP_DIR "x5.fk");
  sqlite3 *d0 = openb(0);
  ex(d0, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT); CREATE TABLE audit(n INTEGER PRIMARY KEY, id INT);"
         "CREATE TRIGGER tr AFTER INSERT ON t BEGIN INSERT INTO audit(id) VALUES(new.id); END;"
         "CREATE TABLE par(id INTEGER PRIMARY KEY); CREATE TABLE chi(id INTEGER PRIMARY KEY, pid INT REFERENCES par(id));"
         "INSERT INTO par VALUES(1),(5)");
  sqlite3_close(d0);
  uint32_t f = fork_of(0), tg = fork_of(0);
  sqlite3 *df = openb(f);
  sqlite3_session *ss; sqlite3session_create(df, "main", &ss); sqlite3session_attach(ss, 0);
  ex(df, "INSERT INTO t VALUES(1,'a'),(2,'b'); INSERT INTO chi VALUES(100, 5)");
  int n = 0; void *p = 0; sqlite3session_changeset(ss, &n, &p);
  printf("  changeset of: 2 inserts into t (a trigger inserts into audit) + 1 child row:\n"); dump_cs(n, p, 10);
  sqlite3 *dt = openb(tg);
  apply(dt, n, p, "target with the same trigger");
  printf("    audit rows after apply: %lld (the fork had %lld)\n", q1(dt, "SELECT count(*) FROM audit"), q1(df, "SELECT count(*) FROM audit"));
  sqlite3_close(dt);
  sqlite3_free(p); sqlite3session_delete(ss); sqlite3_close(df);
  fk_close(S); fk_file_close(&io);
}

/* ---------- X6: promote by replay: conflicts, atomic abort, undo by inversion ---------- */
static void x6(void) {
  printf("== X6: promote by replay, conflicts, abort atomicity, undo by an inverted changeset\n");
  setup(FK_EXP_DIR "x6.fk");
  sqlite3 *d0 = openb(0);
  ex(d0, "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT)"); fillt1(d0, 100); sqlite3_close(d0);
  uint32_t anchor = fork_of(0), f = fork_of(0), f2 = fork_of(0), f3 = fork_of(0);
  (void)anchor;
  sqlite3 *df = openb(f);
  ex(df, "ATTACH 'b1' AS base");
  ex(df, "UPDATE t1 SET v='F' WHERE id IN (10, 20); INSERT INTO t1 VALUES(500,'fnew'); DELETE FROM t1 WHERE id=30");
  sqlite3_session *ss; sqlite3session_create(df, "main", &ss); sqlite3session_attach(ss, "t1"); sqlite3session_diff(ss, "base", "t1", 0);
  int n = 0; void *p = 0; sqlite3session_changeset(ss, &n, &p);
  printf("  F's work (diff against its anchor): "); dump_cs(n, p, 0);
  /* parent moved: disjoint rows */
  sqlite3 *dp = openb(0);
  ex(dp, "UPDATE t1 SET v='P' WHERE id=50");
  conf_ret = SQLITE_CHANGESET_ABORT;
  printf("  (a) parent moved on disjoint rows\n");
  char before[256]; snprintf(before, sizeof before, "%s", qs(dp, "SELECT count(*)||':'||sum(length(v)) FROM t1"));
  apply(dp, n, p, "disjoint"); printf("    parent: %s -> %s\n", before, qs(dp, "SELECT count(*)||':'||sum(length(v)) FROM t1"));
  printf("  (b) undo: invert the changeset and apply\n");
  int ni = 0; void *pi = 0; sqlite3changeset_invert(n, p, &ni, &pi);
  apply(dp, ni, pi, "inverse");
  snprintf(before, sizeof before, "%s", qs(dp, "SELECT v FROM t1 WHERE id=10"));
  printf("    parent back to: %s (id=10 v=%s, id=500 present=%lld)\n", qs(dp, "SELECT count(*)||':'||sum(length(v)) FROM t1"), before, q1(dp, "SELECT count(*) FROM t1 WHERE id=500"));
  printf("  (c) parent changed a row F also changed: replay must abort and leave the parent untouched\n");
  sqlite3 *d2 = openb(f2); ex(d2, "UPDATE t1 SET v='P2' WHERE id=20; UPDATE t1 SET v='P2' WHERE id=60");
  snprintf(before, sizeof before, "%s", qs(d2, "SELECT group_concat(v) FROM (SELECT v FROM t1 WHERE id IN (10,20,60) ORDER BY id)"));
  int rc = apply(d2, n, p, "same row changed on both sides");
  printf("    parent rows 10,20,60: %s -> %s; id=500 present=%lld; in transaction=%d\n", before, qs(d2, "SELECT group_concat(v) FROM (SELECT v FROM t1 WHERE id IN (10,20,60) ORDER BY id)"), q1(d2, "SELECT count(*) FROM t1 WHERE id=500"), !sqlite3_get_autocommit(d2));
  (void)rc;
  printf("  (d) both inserted the same primary key\n");
  sqlite3 *d3 = openb(f3); ex(d3, "INSERT INTO t1 VALUES(500,'other')");
  apply(d3, n, p, "same PK inserted on both sides");
  printf("  (e) the same case with the handler answering OMIT (skip the conflicting change, apply the rest)\n");
  conf_ret = SQLITE_CHANGESET_OMIT; apply(d3, n, p, "omit"); printf("    rows 10,20,500: %s\n", qs(d3, "SELECT group_concat(v) FROM (SELECT v FROM t1 WHERE id IN (10,20,500) ORDER BY id)"));
  conf_ret = SQLITE_CHANGESET_ABORT;
  sqlite3_free(p); sqlite3_free(pi); sqlite3session_delete(ss); sqlite3_close(df); sqlite3_close(dp); sqlite3_close(d2); sqlite3_close(d3);
  fk_close(S); fk_file_close(&io);
}

/* ---------- X7: cost of the diff ---------- */
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void x7(void) {
  printf("== X7: cost of sqlite3session_diff against table size (100 changed rows; 80-byte rows; fkvfs on a real file)\n");
  int sizes[] = {10000, 100000, 1000000};
  for (int k = 0; k < 3; k++) {
    int rows = sizes[k];
    setup(FK_EXP_DIR "x7.fk");
    sqlite3 *d0 = openb(0);
    ex(d0, "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT)");
    ex(d0, "BEGIN");
    sqlite3_stmt *st; sqlite3_prepare_v2(d0, "INSERT INTO t1 VALUES(?, printf('%070d', ?))", -1, &st, 0);
    for (int i = 1; i <= rows; i++) { sqlite3_bind_int(st, 1, i); sqlite3_bind_int(st, 2, i); sqlite3_step(st); sqlite3_reset(st); }
    sqlite3_finalize(st); ex(d0, "COMMIT"); sqlite3_close(d0);
    uint32_t f = fork_of(0); sqlite3 *df = openb(f);
    char sql[128]; snprintf(sql, sizeof sql, "UPDATE t1 SET v='changed' WHERE id %% %d = 0", rows / 100); ex(df, sql);
    ex(df, "ATTACH 'b0' AS base");
    uint32_t pages = 0; fk_size(S, 0, &pages);
    double t0 = now(); long long cnt = q1(df, "SELECT count(*) FROM t1 WHERE v<>'x'"); double t_scan = now() - t0;
    t0 = now(); sqlite3_session *ss; sqlite3session_create(df, "main", &ss); sqlite3session_attach(ss, "t1"); char *err = 0;
    int rc = sqlite3session_diff(ss, "base", "t1", &err); int n = 0; void *p = 0; sqlite3session_changeset(ss, &n, &p); double t_diff = now() - t0;
    printf("  %8d rows (%u pages, %.0f MiB): full scan of one branch %.0f ms; diff rc=%d, %d-byte changeset, %.0f ms\n", rows, pages, pages * 4096.0 / 1048576, t_scan * 1000, rc, n, t_diff * 1000);
    (void)cnt;
    sqlite3_free(p); sqlite3session_delete(ss); sqlite3_close(df);
    fk_close(S); fk_file_close(&io);
  }
}


static void x5b(void) {
  printf("== X5b: replay with the target's triggers dropped inside the same transaction, then re-created\n");
  setup(FK_EXP_DIR "x5b.fk");
  sqlite3 *d0 = openb(0);
  ex(d0, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT); CREATE TABLE audit(n INTEGER PRIMARY KEY, id INT);"
         "CREATE TRIGGER tr AFTER INSERT ON t BEGIN INSERT INTO audit(id) VALUES(new.id); END;");
  sqlite3_close(d0);
  uint32_t f = fork_of(0), tg = fork_of(0);
  sqlite3 *df = openb(f);
  sqlite3_session *ss; sqlite3session_create(df, "main", &ss); sqlite3session_attach(ss, 0);
  ex(df, "INSERT INTO t VALUES(1,'a'),(2,'b')");
  int n = 0; void *p = 0; sqlite3session_changeset(ss, &n, &p);
  sqlite3 *dt = openb(tg);
  ex(dt, "BEGIN");
  char tsql[512]; snprintf(tsql, sizeof tsql, "%s", qs(dt, "SELECT sql FROM sqlite_schema WHERE type='trigger' AND name='tr'"));
  ex(dt, "DROP TRIGGER tr");
  memset(conf_n, 0, sizeof conf_n);
  int rc = sqlite3changeset_apply_v2(dt, n, p, 0, conflict_cb, 0, 0, 0, SQLITE_CHANGESETAPPLY_NOSAVEPOINT);
  printf("  apply_v2 (NOSAVEPOINT) rc=%d, conflicts=%d\n", rc, conf_n[1] + conf_n[2] + conf_n[3] + conf_n[4] + conf_n[5]);
  ex(dt, tsql);
  rc = ex(dt, "COMMIT");
  printf("  commit rc=%d; t rows=%lld, audit rows=%lld (the fork had %lld); trigger present=%lld\n", rc, q1(dt, "SELECT count(*) FROM t"), q1(dt, "SELECT count(*) FROM audit"), q1(df, "SELECT count(*) FROM audit"),
         q1(dt, "SELECT count(*) FROM sqlite_schema WHERE type='trigger'"));
  ex(dt, "INSERT INTO t VALUES(99,'z')");
  printf("  trigger still fires after: audit rows=%lld\n", q1(dt, "SELECT count(*) FROM audit"));
  sqlite3_free(p); sqlite3session_delete(ss); sqlite3_close(df); sqlite3_close(dt);
  fk_close(S); fk_file_close(&io);
}
static void x5c(void) {
  printf("== X5c: foreign keys on apply (no triggers involved)\n");
  setup(FK_EXP_DIR "x5c.fk");
  sqlite3 *d0 = openb(0);
  ex(d0, "CREATE TABLE par(id INTEGER PRIMARY KEY); CREATE TABLE chi(id INTEGER PRIMARY KEY, pid INT REFERENCES par(id)); INSERT INTO par VALUES(1),(5)");
  sqlite3_close(d0);
  uint32_t f = fork_of(0), t1 = fork_of(0), t2 = fork_of(0);
  sqlite3 *df = openb(f);
  sqlite3_session *ss; sqlite3session_create(df, "main", &ss); sqlite3session_attach(ss, 0);
  ex(df, "PRAGMA foreign_keys=ON; INSERT INTO chi VALUES(100, 5)");
  int n = 0; void *p = 0; sqlite3session_changeset(ss, &n, &p);
  sqlite3 *a = openb(t1); ex(a, "PRAGMA foreign_keys=ON; DELETE FROM par WHERE id=5");
  apply(a, n, p, "foreign_keys=ON, parent gone");
  printf("    chi rows after: %lld\n", q1(a, "SELECT count(*) FROM chi"));
  sqlite3 *b = openb(t2); ex(b, "PRAGMA foreign_keys=OFF; DELETE FROM par WHERE id=5");
  apply(b, n, p, "foreign_keys=OFF, parent gone");
  printf("    chi rows after: %lld (an orphan if 1)\n", q1(b, "SELECT count(*) FROM chi"));
  sqlite3_free(p); sqlite3session_delete(ss); sqlite3_close(df); sqlite3_close(a); sqlite3_close(b);
  fk_close(S); fk_file_close(&io);
}
int main(int argc, char **argv) {
  sqlite3_config(SQLITE_CONFIG_LOG, logcb, 0);
  const char *w = argc > 1 ? argv[1] : "all";
  if (!strcmp(w, "x1") || !strcmp(w, "all")) x1();
  if (!strcmp(w, "x2") || !strcmp(w, "all")) x2();
  if (!strcmp(w, "x4") || !strcmp(w, "all")) x4();
  if (!strcmp(w, "x5") || !strcmp(w, "all")) x5();
  if (!strcmp(w, "x5b") || !strcmp(w, "all")) x5b();
  if (!strcmp(w, "x5c") || !strcmp(w, "all")) x5c();
  if (!strcmp(w, "x6") || !strcmp(w, "all")) x6();
  if (!strcmp(w, "x7")) x7();
  return 0;
}
