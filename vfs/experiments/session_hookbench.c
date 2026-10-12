/* Cost of compiling SQLite with the session extension: bulk insert in memory, and single-row commits through the VFS, with the session
 * attached to every table (on) or not (off). Built three ways by session-experiments.sh bench. Prints milliseconds. See docs/SESSION-DESIGN.md. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef FK_EXP_DIR
#define FK_EXP_DIR "/tmp/forkdb-session-exp/"
#endif
#include "sqlite3.h"
#ifdef WITH_SESSION
#include "sqlite3session.h"
#endif
#include "fkvfs.h"
#include "fkstore.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void ex(sqlite3 *db, const char *s) { char *e = 0; if (sqlite3_exec(db, s, 0, 0, &e)) { printf("err %s: %s\n", s, e); exit(1); } }
int main(int argc, char **argv) {
  int attach = argc > 1 && !strcmp(argv[1], "on");
  const char *mode = argc > 2 ? argv[2] : "mem";
  double t0, t1;
  if (!strcmp(mode, "mem")) {
    sqlite3 *db; sqlite3_open(":memory:", &db);
    ex(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, a INT, b TEXT); CREATE INDEX ta ON t(a)");
#ifdef WITH_SESSION
    sqlite3_session *ss = 0; if (attach) { sqlite3session_create(db, "main", &ss); sqlite3session_attach(ss, 0); }
#endif
    t0 = now(); ex(db, "BEGIN");
    sqlite3_stmt *st; sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?, ?, 'row-text-0123456789')", -1, &st, 0);
    for (int i = 1; i <= 1000000; i++) { sqlite3_bind_int(st, 1, i); sqlite3_bind_int(st, 2, (int)(((long long)i * 7919) % 100003)); sqlite3_step(st); sqlite3_reset(st); }
    sqlite3_finalize(st);
    ex(db, "UPDATE t SET b='u' WHERE id%3=0"); ex(db, "COMMIT");
    t1 = now(); printf("%.1f\n", (t1 - t0) * 1000);
  } else {
    remove(FK_EXP_DIR "hook.fk");
    fk_io io; fk_store *S; FkVfs *V;
    fk_file_open(FK_EXP_DIR "hook.fk", &io); fk_open(&io, &S); fkvfs_register(S, "forkdb", &V);
    sqlite3 *db; sqlite3_open_v2("b0", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "forkdb");
    ex(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, a INT, b TEXT)");
#ifdef WITH_SESSION
    sqlite3_session *ss = 0; if (attach) { sqlite3session_create(db, "main", &ss); sqlite3session_attach(ss, 0); }
#endif
    t0 = now();
    for (int i = 1; i <= 3000; i++) { char sql[128]; snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, %d, 'row-text-0123456789')", i, i * 3); ex(db, sql); }
    t1 = now(); printf("%.1f\n", (t1 - t0) * 1000);
  }
  return 0;
}
