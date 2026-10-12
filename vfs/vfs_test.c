/* Tests for fkvfs: SQLite on the durable store. Build and run with scripts/test-vfs.sh. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../test/simdisk.h"
#include "fkvfs.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int have_batch(void) { return sqlite3_compileoption_used("ENABLE_BATCH_ATOMIC_WRITE"); }

static int sql(sqlite3 *db, const char *q) {
  char *e = NULL;
  int rc = sqlite3_exec(db, q, 0, 0, &e);
  if (e && getenv("VFS_VERBOSE")) printf("   sql error (%d): %s\n", rc, e);
  sqlite3_free(e);
  return rc;
}

static long long scalar(sqlite3 *db, const char *q) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db, q, -1, &st, 0) != SQLITE_OK) return -1;
  long long v = -1;
  if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

static int text_is(sqlite3 *db, const char *q, const char *want, char *got, size_t n) {
  sqlite3_stmt *st;
  got[0] = 0;
  if (sqlite3_prepare_v2(db, q, -1, &st, 0) != SQLITE_OK) return 0;
  if (sqlite3_step(st) == SQLITE_ROW) { const unsigned char *t = sqlite3_column_text(st, 0); if (t) snprintf(got, n, "%s", (const char *)t); }
  sqlite3_finalize(st);
  return want && strcmp(got, want) == 0;
}

static int integrity_ok(sqlite3 *db) { char g[64]; return text_is(db, "PRAGMA integrity_check", "ok", g, sizeof g); }

static sqlite3 *openb(const char *vfs, uint32_t b) {
  char nm[32];
  sqlite3 *db = NULL;
  fkvfs_branch_name(b, nm, sizeof nm);
  if (sqlite3_open_v2(nm, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs) != SQLITE_OK) { sqlite3_close(db); return NULL; }
  return db;
}

/* a store in a real file, with the VFS over it */
typedef struct { fk_io io; fk_store *s; FkVfs *v; char path[320]; int allow_err; } Env;

static int env_open(Env *e, const char *path) {
  memset(e, 0, sizeof *e);
  snprintf(e->path, sizeof e->path, "%s", path);
  if (fk_file_open(path, &e->io) != 0 || fk_open(&e->io, &e->s) != 0) return -1;
  return fkvfs_register(e->s, "forkdb", &e->v);
}
static void env_close(Env *e) {
  FkVfsStats st;
  fkvfs_stats(e->v, &st);
  CHECK(st.unlock_publish_errors == 0, "no publish failed silently at unlock (%llu did)", (unsigned long long)st.unlock_publish_errors);
  CHECK(e->allow_err || st.sync_publish_errors == 0, "no publish failed at xSync, where a rollback can swallow the error (%llu did)", (unsigned long long)st.sync_publish_errors);
  int rc = fkvfs_unregister(e->v);
  CHECK(rc == SQLITE_OK, "unregister: all connections were closed (rc=%d)", rc);
  fk_close(e->s);
  fk_file_close(&e->io);
}

static char dir[256];
static const char *P(const char *name, char *buf, size_t n) { snprintf(buf, n, "%s/%s", dir, name); return buf; }

static void fill(sqlite3 *db, const char *table, int n, int payload) {
  char q[400];
  snprintf(q, sizeof q, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<%d) INSERT INTO %s(v) SELECT randomblob(%d) FROM c", n, table, payload);
  CHECK(sql(db, q) == 0, "fill %s", table);
}

static void pstats(FkVfs *v, const char *when) {
  FkVfsStats s;
  fkvfs_stats(v, &s);
  printf("   %s: begin=%llu commit=%llu rollback=%llu journals=%llu sync-publish=%llu unlock-publish=%llu store-commits=%llu\n", when, (unsigned long long)s.atomic_begin, (unsigned long long)s.atomic_commit, (unsigned long long)s.atomic_rollback, (unsigned long long)s.journal_opens, (unsigned long long)s.sync_publish, (unsigned long long)s.unlock_publish, (unsigned long long)s.store_commits);
}

/* ---------------------------------------------------------------- */
static void t_basic(int batch, int try_off) {
  printf("T1 basic and persistence (batch advertised=%d, sqlite has it=%d%s)\n", batch, have_batch(), try_off ? ", PRAGMA synchronous=OFF refused" : "");
  char pp[400];
  Env e;
  CHECK(env_open(&e, P("t1.fk", pp, sizeof pp)) == 0, "open");
  fkvfs_set_batch_atomic(e.v, batch);
  sqlite3 *db = openb("forkdb", 0);
  CHECK(db != NULL, "open b0");
  if (!db) return;
  if (try_off) {
    CHECK(sql(db, "PRAGMA synchronous=OFF") != SQLITE_OK && sql(db, "PRAGMA synchronous=0") != SQLITE_OK, "synchronous=OFF is refused");
    CHECK(scalar(db, "PRAGMA synchronous") == 2 && sql(db, "PRAGMA synchronous=NORMAL") == SQLITE_OK, "the setting is still FULL, and NORMAL is allowed");
  }
  CHECK(sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)") == 0, "create");
  pstats(e.v, "after CREATE TABLE (the first transaction on an empty database)");
  FkVfsStats sa;
  fkvfs_stats(e.v, &sa);
  CHECK(sa.store_commits == 1, "the first transaction is one store commit, whichever path it takes");
  CHECK(sql(db, "BEGIN") == 0, "begin");
  fill(db, "t", 100, 50);
  CHECK(sql(db, "COMMIT") == 0, "commit");
  pstats(e.v, "after a 100-row insert");
  CHECK(scalar(db, "SELECT count(*) FROM t") == 100, "100 rows");
  FkVfsStats st;
  fkvfs_stats(e.v, &st);
  if (batch && have_batch()) CHECK(st.atomic_commit == sa.atomic_commit + 1 && st.journal_opens == sa.journal_opens && st.sync_publish == sa.sync_publish, "the 100-row insert takes the batch path: commits +%llu journals +%llu sync +%llu", (unsigned long long)(st.atomic_commit - sa.atomic_commit), (unsigned long long)(st.journal_opens - sa.journal_opens), (unsigned long long)(st.sync_publish - sa.sync_publish));
  else CHECK(st.atomic_begin == 0 && st.sync_publish >= 2 && st.unlock_publish == 0, "journal path: begin=%llu sync=%llu unlock=%llu", (unsigned long long)st.atomic_begin, (unsigned long long)st.sync_publish, (unsigned long long)st.unlock_publish);
  sqlite3_close(db);
  env_close(&e);
  CHECK(env_open(&e, pp) == 0, "reopen the store");
  db = openb("forkdb", 0);
  CHECK(db && scalar(db, "SELECT count(*) FROM t") == 100 && integrity_ok(db), "100 rows survive a close and reopen, integrity ok");
  sqlite3_close(db);
  env_close(&e);
  unlink(pp);
}

static void t_fork(void) {
  printf("T2 fork isolation\n");
  char pp[400];
  Env e;
  CHECK(env_open(&e, P("t2.fk", pp, sizeof pp)) == 0, "open");
  sqlite3 *a = openb("forkdb", 0);
  CHECK(sql(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)") == 0, "create");
  fill(a, "t", 1000, 200);
  uint32_t pages0 = 0;
  fk_size(e.s, 0, &pages0);
  uint64_t fb0, lb0;
  fk_stats(e.s, &fb0, &lb0);
  uint32_t b1 = 0;
  CHECK(fk_fork(e.s, 0, &b1) == FK_OK && b1 == 1, "fork");
  sqlite3 *b = openb("forkdb", b1);
  CHECK(b && scalar(b, "SELECT count(*) FROM t") == 1000, "fork starts as a copy");
  uint64_t fb1, lb1;
  fk_stats(e.s, &fb1, &lb1);
  CHECK(sql(b, "INSERT INTO t(v) VALUES (x'00')") == 0 && sql(b, "UPDATE t SET v=x'01' WHERE id<=10") == 0, "write on the fork");
  uint64_t fb2, lb2;
  fk_stats(e.s, &fb2, &lb2);
  printf("   %u pages in the database; fork cost %lld bytes of log; a one-row insert plus a 10-row update on the fork cost %lld bytes of log\n", pages0, (long long)(fb1 - fb0), (long long)(fb2 - fb1));
  CHECK(fb2 - fb1 <= 12u * FK_PAGE, "the write touched a handful of pages, not the database");
  CHECK(scalar(b, "SELECT count(*) FROM t") == 1001, "fork sees its write");
  CHECK(scalar(a, "SELECT count(*) FROM t") == 1000 && scalar(a, "SELECT count(*) FROM t WHERE v=x'01'") == 0, "parent does not see the fork's writes (its connection re-reads: the change counter moved only on the fork)");
  CHECK(sql(a, "INSERT INTO t(v) VALUES (x'02')") == 0 && sql(a, "INSERT INTO t(v) VALUES (x'03')") == 0, "write on the parent after the fork");
  CHECK(scalar(b, "SELECT count(*) FROM t") == 1001 && scalar(b, "SELECT count(*) FROM t WHERE v IN (x'02', x'03')") == 0, "fork does not see the parent's later writes");
  uint32_t b2 = 0;
  CHECK(fk_fork(e.s, b1, &b2) == FK_OK, "fork of a fork");
  sqlite3 *c = openb("forkdb", b2);
  CHECK(c && scalar(c, "SELECT count(*) FROM t") == 1001, "grandchild is a copy of the child");
  sqlite3_close(b);
  CHECK(fk_discard(e.s, b1) == FK_OK, "discard the middle branch");
  CHECK(scalar(c, "SELECT count(*) FROM t") == 1001 && integrity_ok(c), "grandchild still reads fine");
  CHECK(sql(c, "DELETE FROM t WHERE id>500") == 0 && scalar(c, "SELECT count(*) FROM t") == 500, "write on the grandchild");
  CHECK(scalar(a, "SELECT count(*) FROM t") == 1002, "parent untouched by the grandchild");
  sqlite3_close(a);
  sqlite3_close(c);
  env_close(&e);
  /* everything survives a reopen */
  CHECK(env_open(&e, pp) == 0, "reopen");
  a = openb("forkdb", 0);
  c = openb("forkdb", b2);
  CHECK(a && c && scalar(a, "SELECT count(*) FROM t") == 1002 && scalar(c, "SELECT count(*) FROM t") == 500 && integrity_ok(a) && integrity_ok(c), "branches after reopen");
  sqlite3_close(a);
  sqlite3_close(c);
  env_close(&e);
  unlink(pp);
}

static void t_fallback(void) {
  printf("T3 a refused batch commit falls back to the journal and the transaction succeeds\n");
  if (!have_batch()) { printf("   skipped: this SQLite was built without SQLITE_ENABLE_BATCH_ATOMIC_WRITE\n"); return; }
  char pp[400];
  Env e;
  CHECK(env_open(&e, P("t3.fk", pp, sizeof pp)) == 0, "open");
  sqlite3 *db = openb("forkdb", 0);
  CHECK(sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)") == 0, "create");
  FkVfsStats s0, s1;
  fkvfs_stats(e.v, &s0);
  fkvfs_fail_commits(e.v, 1);
  CHECK(sql(db, "INSERT INTO t(v) VALUES (x'aa')") == 0, "the insert succeeds although the first commit was refused");
  fkvfs_stats(e.v, &s1);
  CHECK(s1.commit_failed == s0.commit_failed + 1 && s1.journal_opens == s0.journal_opens + 1 && s1.sync_publish == s0.sync_publish + 1, "refused=%llu journals+%llu sync+%llu", (unsigned long long)(s1.commit_failed - s0.commit_failed), (unsigned long long)(s1.journal_opens - s0.journal_opens), (unsigned long long)(s1.sync_publish - s0.sync_publish));
  CHECK(sql(db, "INSERT INTO t(v) VALUES (x'bb')") == 0, "the next one takes the batch path again");
  sqlite3_close(db);
  env_close(&e);
  CHECK(env_open(&e, pp) == 0, "reopen");
  db = openb("forkdb", 0);
  CHECK(db && scalar(db, "SELECT count(*) FROM t") == 2 && integrity_ok(db), "both rows are there");
  sqlite3_close(db);
  env_close(&e);
  unlink(pp);
}

static void t_spill(int batch) {
  printf("T4 a transaction bigger than the page cache: rollback leaves no trace, commit is complete (batch advertised=%d)\n", batch);
  char pp[400];
  Env e;
  CHECK(env_open(&e, P("t4.fk", pp, sizeof pp)) == 0, "open");
  fkvfs_set_batch_atomic(e.v, batch);
  sqlite3 *db = openb("forkdb", 0);
  CHECK(sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)") == 0, "create");
  fill(db, "t", 200, 100);
  CHECK(sql(db, "PRAGMA cache_size=-64") == 0, "small page cache (64 KiB)");
  FkVfsStats s0, s1, s2;
  uint32_t pg0, pg1;
  uint64_t f0, f1, l, deduped, unlocks, syncs;
  fk_size(e.s, 0, &pg0);
  fk_stats(e.s, &f0, &l);
  fkvfs_stats(e.v, &s0);
  CHECK(sql(db, "BEGIN") == 0, "begin");
  fill(db, "t", 20000, 100);
  CHECK(scalar(db, "SELECT count(*) FROM t") == 20200, "20200 rows inside the transaction");
  CHECK(sql(db, "ROLLBACK") == 0, "rollback");
  fkvfs_stats(e.v, &s1);
  fk_size(e.s, 0, &pg1);
  fk_stats(e.s, &f1, &l);
  printf("   rollback: store commits +%llu, pages published +%llu, deduped +%llu, journals +%llu, log grew %lld bytes\n", (unsigned long long)(s1.store_commits - s0.store_commits), (unsigned long long)(s1.pages_published - s0.pages_published), (unsigned long long)(s1.pages_deduped - s0.pages_deduped), (unsigned long long)(s1.journal_opens - s0.journal_opens), (long long)(f1 - f0));
  CHECK(scalar(db, "SELECT count(*) FROM t") == 200 && integrity_ok(db), "back to 200 rows, integrity ok");
  fkvfs_stats(e.v, &s2);
  CHECK(scalar(db, "SELECT count(*) FROM t") == 200, "read again after the rollback");
  deduped = s2.pages_deduped; unlocks = s2.unlock_publish; syncs = s2.sync_publish;
  fkvfs_stats(e.v, &s2);
  CHECK(s2.pages_deduped == deduped && s2.unlock_publish == unlocks && s2.sync_publish == syncs, "the no-op publish cleared staging: a later lock release did not scan it again (deduped +%llu, unlock-publish +%llu, sync-publish +%llu)", (unsigned long long)(s2.pages_deduped - deduped), (unsigned long long)(s2.unlock_publish - unlocks), (unsigned long long)(s2.sync_publish - syncs));
  CHECK(pg1 == pg0, "database size unchanged (%u vs %u pages)", pg1, pg0);
  CHECK(s1.store_commits == s0.store_commits, "a rolled-back transaction commits nothing to the store (%llu commits)", (unsigned long long)(s1.store_commits - s0.store_commits));
  CHECK(sql(db, "BEGIN") == 0, "begin");
  fill(db, "t", 20000, 100);
  CHECK(sql(db, "COMMIT") == 0, "commit");
  fkvfs_stats(e.v, &s1);
  printf("   commit of 20000 rows: store commits +%llu, pages published +%llu, journals +%llu\n", (unsigned long long)(s1.store_commits - s0.store_commits), (unsigned long long)(s1.pages_published - s0.pages_published), (unsigned long long)(s1.journal_opens - s0.journal_opens));
  CHECK(scalar(db, "SELECT count(*) FROM t") == 20200 && integrity_ok(db), "20200 rows");
  sqlite3_close(db);
  env_close(&e);
  CHECK(env_open(&e, pp) == 0, "reopen");
  db = openb("forkdb", 0);
  CHECK(db && scalar(db, "SELECT count(*) FROM t") == 20200 && integrity_ok(db), "20200 rows after reopen");
  sqlite3_close(db);
  env_close(&e);
  unlink(pp);
}

static void t_failed_publish(void) {
  printf("T12 a publish that fails part way (the store took some pages): the transaction fails, and the connection and the branch stay sound\n");
  char pp[400];
  Env e;
  CHECK(env_open(&e, P("t12.fk", pp, sizeof pp)) == 0, "open");
  e.allow_err = 1; /* the failures here are injected */
  fkvfs_set_batch_atomic(e.v, 0); /* the journal path: the publish happens at xSync */
  sqlite3 *db = openb("forkdb", 0);
  CHECK(sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)") == 0, "create");
  fill(db, "t", 200, 100);
  FkVfsStats s0, s1;
  fkvfs_stats(e.v, &s0);
  fkvfs_fail_publish_page(e.v, 2);
  CHECK(sql(db, "BEGIN") == 0, "begin");
  fill(db, "t", 300, 100);
  int rc = sql(db, "COMMIT");
  CHECK(rc != SQLITE_OK, "the commit fails (rc=%d)", rc);
  sql(db, "ROLLBACK"); /* the transaction may already be over */
  fkvfs_stats(e.v, &s1);
  CHECK(s1.sync_publish_errors == s0.sync_publish_errors + 1, "the failure was at xSync (%llu)", (unsigned long long)(s1.sync_publish_errors - s0.sync_publish_errors));
  CHECK(scalar(db, "SELECT count(*) FROM t") == 200 && integrity_ok(db), "back to 200 rows, integrity ok");
  CHECK(sql(db, "INSERT INTO t(v) VALUES (x'01')") == 0 && scalar(db, "SELECT count(*) FROM t") == 201, "the connection is usable, and the next transaction commits");
  sqlite3_close(db);
  env_close(&e);
  CHECK(env_open(&e, pp) == 0, "reopen");
  db = openb("forkdb", 0);
  CHECK(db && scalar(db, "SELECT count(*) FROM t") == 201 && integrity_ok(db), "201 rows after a reopen: the failed transaction left nothing");
  sqlite3_close(db);
  env_close(&e);
  unlink(pp);
}

static int write_filled(sqlite3_file *f, uint32_t pg, uint8_t byte) {
  uint8_t buf[FK_PAGE];
  memset(buf, byte, sizeof buf);
  return f->pMethods->xWrite(f, buf, (int)FK_PAGE, (sqlite3_int64)pg * FK_PAGE);
}

static int page_is(sqlite3_file *f, uint32_t pg, uint8_t byte) {
  uint8_t buf[FK_PAGE];
  if (f->pMethods->xRead(f, buf, (int)FK_PAGE, (sqlite3_int64)pg * FK_PAGE) != SQLITE_OK) return 0;
  for (unsigned i = 0; i < FK_PAGE; i++) if (buf[i] != byte) return 0;
  return 1;
}

/* Truncate, then write a page back inside the old size. Publish must keep that page and zero only the hole. */
static void t_trunc_rewrite(void) {
  printf("T13 a page written back into a truncated range survives publish\n");
  char pp[400];
  Env e;
  int opened = env_open(&e, P("t13.fk", pp, sizeof pp));
  CHECK(opened == 0, "open");
  if (opened != 0) return;
  sqlite3_vfs *vfs = sqlite3_vfs_find("forkdb");
  CHECK(vfs != NULL, "vfs is registered");
  sqlite3_file *f = vfs ? calloc(1, (size_t)vfs->szOsFile) : NULL;
  CHECK(f != NULL, "file buffer");
  int orc = f ? vfs->xOpen(vfs, "b0", f, SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_READWRITE, NULL) : SQLITE_NOMEM;
  CHECK(orc == SQLITE_OK, "open b0 (rc=%d)", orc);
  if (orc == SQLITE_OK) {
    for (uint32_t pg = 0; pg < 6; pg++) CHECK(write_filled(f, pg, (uint8_t)(0x10 + pg)) == SQLITE_OK, "write page %u", pg);
    CHECK(f->pMethods->xSync(f, SQLITE_SYNC_NORMAL) == SQLITE_OK, "commit six pages");
    CHECK(f->pMethods->xTruncate(f, 2 * (sqlite3_int64)FK_PAGE) == SQLITE_OK, "truncate to 2 pages");
    CHECK(write_filled(f, 0, 0xB0) == SQLITE_OK, "rewrite a page below the truncate point");
    CHECK(write_filled(f, 2, 0xB2) == SQLITE_OK, "rewrite the first page of the hole");
    CHECK(write_filled(f, 4, 0xB4) == SQLITE_OK, "write back further into the old size");
    CHECK(f->pMethods->xSync(f, SQLITE_SYNC_NORMAL) == SQLITE_OK, "publish the truncate and the rewritten pages");
    sqlite3_int64 sz = -1;
    CHECK(f->pMethods->xFileSize(f, &sz) == SQLITE_OK && sz == 5 * (sqlite3_int64)FK_PAGE, "size is 5 pages (%lld)", (long long)sz);
    CHECK(page_is(f, 0, 0xB0), "page 0 is the rewrite below the truncate point");
    CHECK(page_is(f, 1, 0x11), "page 1 is unchanged");
    CHECK(page_is(f, 2, 0xB2), "page 2 keeps the bytes written into the truncated range");
    CHECK(page_is(f, 3, 0x00), "page 3, the hole, is zeros");
    CHECK(page_is(f, 4, 0xB4), "page 4 keeps the bytes written into the re-expanded area");
    f->pMethods->xClose(f);
  }
  free(f);
  env_close(&e);
  unlink(pp);
}

static void t_pagesize(void) {
  printf("T5 page sizes\n");
  char pp[400];
  Env e;
  CHECK(env_open(&e, P("t5.fk", pp, sizeof pp)) == 0, "open");
  const int sizes[] = {4096, 8192, 65536};
  uint32_t br[4] = {0, 0, 0, 0}; /* branch 0 is empty, so forks of it are independent empty databases */
  for (int i = 1; i < 4; i++) CHECK(fk_fork(e.s, 0, &br[i]) == FK_OK, "new branch");
  for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
    uint32_t b = br[i];
    sqlite3 *db = openb("forkdb", b);
    char q[64];
    snprintf(q, sizeof q, "PRAGMA page_size=%d", sizes[i]);
    CHECK(sql(db, q) == 0, "set page size");
    CHECK(sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)") == 0, "create");
    fill(db, "t", 500, 300);
    CHECK(scalar(db, "PRAGMA page_size") == sizes[i] && scalar(db, "SELECT count(*) FROM t") == 500 && integrity_ok(db), "page size %d works", sizes[i]);
    sqlite3_close(db);
  }
  sqlite3 *db = openb("forkdb", br[3]);
  sql(db, "PRAGMA page_size=1024");
  int rc = sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)");
  printf("   page_size=1024: CREATE TABLE returns %d (%s)\n", rc, rc ? sqlite3_errstr(rc) : "ok");
  CHECK(rc != SQLITE_OK, "a page size below 4096 is refused, not silently corrupted");
  sqlite3_close(db);
  env_close(&e);
  unlink(pp);
}

static void t_modes(void) {
  printf("T6 journal modes; WAL is refused\n");
  char pp[400];
  Env e;
  CHECK(env_open(&e, P("t6.fk", pp, sizeof pp)) == 0, "open");
  const char *modes[] = {"DELETE", "TRUNCATE", "PERSIST", "MEMORY", "OFF"};
  fkvfs_set_batch_atomic(e.v, 0); /* the journal modes only matter on the journal path */
  uint32_t br[6] = {0, 0, 0, 0, 0, 0}; /* branch 0 is still empty: the forks are independent empty databases */
  for (int i = 1; i < 6; i++) CHECK(fk_fork(e.s, 0, &br[i]) == FK_OK, "branch");
  for (size_t i = 0; i < 5; i++) {
    sqlite3 *db = openb("forkdb", br[i]);
    char q[64], g[64];
    snprintf(q, sizeof q, "PRAGMA journal_mode=%s", modes[i]);
    CHECK(text_is(db, q, NULL, g, sizeof g) || 1, "set mode");
    CHECK(strcasecmp(g, modes[i]) == 0, "journal_mode=%s is accepted (got '%s')", modes[i], g);
    CHECK(sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)") == 0, "create");
    CHECK(sql(db, "BEGIN") == 0, "begin");
    fill(db, "t", 300, 100);
    CHECK(sql(db, "COMMIT") == 0 && scalar(db, "SELECT count(*) FROM t") == 300 && integrity_ok(db), "mode %s", modes[i]);
    if (i != 4) { /* rollback is undefined with journal_mode=OFF */
      CHECK(sql(db, "BEGIN") == 0 && sql(db, "DELETE FROM t") == 0 && sql(db, "ROLLBACK") == 0 && scalar(db, "SELECT count(*) FROM t") == 300, "rollback in %s", modes[i]);
    }
    sqlite3_close(db);
  }
  sqlite3 *db = openb("forkdb", br[5]);
  char g[64];
  int ok = text_is(db, "PRAGMA journal_mode=WAL", "wal", g, sizeof g);
  printf("   PRAGMA journal_mode=WAL answers '%s'\n", g);
  CHECK(!ok, "WAL must not be enabled (it would live in memory and a commit would not be durable)");
  CHECK(sql(db, "CREATE TABLE t(x)") == 0 && sql(db, "INSERT INTO t VALUES (1)") == 0, "the database still works after the refused PRAGMA");
  sqlite3_close(db);
  /* SQLite allows WAL without shared memory when the connection is in exclusive locking mode, so the VFS itself must refuse the -wal file */
  uint32_t bx = 0;
  CHECK(fk_fork(e.s, 0, &bx) == FK_OK, "branch");
  db = openb("forkdb", bx);
  CHECK(sql(db, "PRAGMA locking_mode=EXCLUSIVE") == 0, "exclusive locking mode");
  ok = text_is(db, "PRAGMA journal_mode=WAL", "wal", g, sizeof g);
  printf("   in exclusive locking mode PRAGMA journal_mode=WAL answers '%s'\n", g);
  CHECK(!ok, "WAL must not be enabled in exclusive locking mode either");
  CHECK(sql(db, "CREATE TABLE u(x)") == 0 && sql(db, "INSERT INTO u VALUES (1)") == 0, "still usable");
  sqlite3_close(db);
  uint8_t hdr[FK_PAGE];
  CHECK(fk_read(e.s, bx, 0, hdr) == FK_OK && hdr[18] == 1 && hdr[19] == 1, "the header still says rollback journal (bytes 18 and 19 are %d and %d)", hdr[18], hdr[19]);
  db = openb("forkdb", bx);
  CHECK(db && scalar(db, "SELECT count(*) FROM u") == 1, "and the branch opens again");
  sqlite3_close(db);
  env_close(&e);
  unlink(pp);
}

static void t_conns(int batch) {
  printf("T7 two connections on one branch: locking (batch advertised=%d)\n", batch);
  char pp[400];
  Env e;
  CHECK(env_open(&e, P("t7.fk", pp, sizeof pp)) == 0, "open");
  fkvfs_set_batch_atomic(e.v, batch);
  sqlite3 *a = openb("forkdb", 0), *b = openb("forkdb", 0);
  CHECK(sql(a, "CREATE TABLE t(x)") == 0 && sql(a, "INSERT INTO t VALUES (1)") == 0, "setup");
  CHECK(scalar(b, "SELECT count(*) FROM t") == 1, "second connection reads");
  CHECK(sql(a, "BEGIN IMMEDIATE") == 0 && sql(a, "INSERT INTO t VALUES (2)") == 0, "a holds the write lock");
  CHECK(sql(b, "BEGIN IMMEDIATE") == SQLITE_BUSY, "b cannot also write");
  CHECK(scalar(b, "SELECT count(*) FROM t") == 1, "b still reads the old state while a has an open write transaction");
  CHECK(sql(a, "COMMIT") == 0, "a commits");
  CHECK(scalar(b, "SELECT count(*) FROM t") == 2, "b sees the commit");
  /* a reader holding a read transaction delays a writer's commit until the reader is done */
  CHECK(sql(b, "BEGIN") == 0 && scalar(b, "SELECT count(*) FROM t") == 2, "b holds a read transaction");
  CHECK(sql(a, "BEGIN IMMEDIATE") == 0 && sql(a, "INSERT INTO t VALUES (3)") == 0, "a writes");
  int rc = sql(a, "COMMIT");
  CHECK(rc == SQLITE_BUSY, "a cannot commit while b reads (rc=%d)", rc);
  CHECK(sql(b, "COMMIT") == 0, "b ends its read transaction");
  CHECK(sql(a, "COMMIT") == 0, "a commits now");
  CHECK(scalar(b, "SELECT count(*) FROM t") == 3 && integrity_ok(a), "3 rows");
  /* closing with an open transaction leaves nothing behind */
  CHECK(sql(a, "BEGIN IMMEDIATE") == 0 && sql(a, "INSERT INTO t VALUES (4)") == 0, "open transaction");
  sqlite3_close(a);
  CHECK(scalar(b, "SELECT count(*) FROM t") == 3, "closing a connection with an open transaction rolls it back");
  sqlite3_close(b);
  env_close(&e);
  unlink(pp);
}

static void t_vacuum(void) {
  printf("T8 VACUUM and shrinking\n");
  char pp[400];
  Env e;
  CHECK(env_open(&e, P("t8.fk", pp, sizeof pp)) == 0, "open");
  sqlite3 *db = openb("forkdb", 0);
  CHECK(sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)") == 0, "create");
  fill(db, "t", 5000, 500);
  uint32_t big = 0, small = 0, mid = 0;
  fk_size(e.s, 0, &big);
  CHECK(sql(db, "DELETE FROM t WHERE id>100") == 0, "delete");
  fk_size(e.s, 0, &mid);
  CHECK(sql(db, "VACUUM") == 0, "vacuum");
  fk_size(e.s, 0, &small);
  printf("   pages: %u full, %u after the delete, %u after VACUUM\n", big, mid, small);
  CHECK(small < big / 10 && scalar(db, "SELECT count(*) FROM t") == 100 && integrity_ok(db), "VACUUM shrank the database");
  fill(db, "t", 3000, 500);
  CHECK(scalar(db, "SELECT count(*) FROM t") == 3100 && integrity_ok(db), "grows again");
  CHECK(sql(db, "PRAGMA auto_vacuum=FULL") == 0 && sql(db, "VACUUM") == 0 && sql(db, "DELETE FROM t WHERE id>50") == 0, "auto_vacuum=FULL shrinks inside the transaction");
  CHECK(scalar(db, "SELECT count(*) FROM t") == 50 && integrity_ok(db), "50 rows, integrity ok");
  sqlite3_close(db);
  env_close(&e);
  CHECK(env_open(&e, pp) == 0, "reopen");
  db = openb("forkdb", 0);
  CHECK(db && scalar(db, "SELECT count(*) FROM t") == 50 && integrity_ok(db), "after reopen");
  sqlite3_close(db);
  env_close(&e);
  unlink(pp);
}

static void t_temp(void) {
  printf("T9 temporary files, savepoints, big sorts\n");
  char pp[400];
  Env e;
  CHECK(env_open(&e, P("t9.fk", pp, sizeof pp)) == 0, "open");
  sqlite3 *db = openb("forkdb", 0);
  CHECK(sql(db, "PRAGMA temp_store=FILE") == 0 && sql(db, "PRAGMA cache_size=-64") == 0, "temp files in memory files, small cache");
  CHECK(sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)") == 0, "create");
  fill(db, "t", 20000, 100);
  CHECK(scalar(db, "SELECT count(*) FROM (SELECT v FROM t ORDER BY v)") == 20000, "sort that spills to a temporary file");
  CHECK(sql(db, "CREATE TEMP TABLE tt AS SELECT * FROM t WHERE id%2=0") == 0 && scalar(db, "SELECT count(*) FROM tt") == 10000, "temp table");
  CHECK(sql(db, "BEGIN") == 0 && sql(db, "SAVEPOINT a") == 0 && sql(db, "DELETE FROM t") == 0 && sql(db, "ROLLBACK TO a") == 0 && sql(db, "INSERT INTO t(v) VALUES (x'01')") == 0 && sql(db, "COMMIT") == 0, "savepoints");
  CHECK(scalar(db, "SELECT count(*) FROM t") == 20001 && integrity_ok(db), "20001 rows");
  sqlite3_close(db);
  env_close(&e);
  unlink(pp);
}

static void t_names(void) {
  printf("T10 names and dead branches\n");
  char pp[400];
  Env e;
  CHECK(env_open(&e, P("t10.fk", pp, sizeof pp)) == 0, "open");
  sqlite3 *db = NULL;
  CHECK(sqlite3_open_v2("foo", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "forkdb") != SQLITE_OK || 1, "bad name");
  int rc = sql(db, "CREATE TABLE t(x)");
  CHECK(rc != SQLITE_OK, "a name that is not b<N> cannot be used (rc=%d)", rc);
  sqlite3_close(db);
  db = openb("forkdb", 7);
  rc = db ? sql(db, "SELECT count(*) FROM sqlite_master") : SQLITE_CANTOPEN; /* sqlite3_open_v2 itself refuses */
  CHECK(rc == SQLITE_CANTOPEN, "a branch that does not exist cannot be opened, not even to read (rc=%d)", rc);
  sqlite3_close(db);
  db = NULL;
  sqlite3_open_v2("b0", &db, SQLITE_OPEN_READONLY, "forkdb");
  CHECK(db != NULL, "read-only open of an empty branch 0");
  sqlite3_close(db);
  env_close(&e);
  unlink(pp);
}

/* ---------------------------------------------------------------- power loss: SQLite on the simulated disk */
#define NT 7
static int rows_for(int j) { return j % 3 == 0 ? 40 : 3; }

typedef struct { int ack0, ack1, forked; } Ack;

static int do_txn(sqlite3 *db, int branch, int i) {
  char q[600];
  if (sql(db, "BEGIN")) return -1;
  snprintf(q, sizeof q, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<%d) INSERT INTO kv(txn, tag, pad) SELECT %d, %d, randomblob(%d) FROM c", rows_for(i), i, branch, (i * 97) % 1500 + 10);
  if (sql(db, q)) { sql(db, "ROLLBACK"); return -1; }
  snprintf(q, sizeof q, "UPDATE meta SET last=%d", i);
  if (sql(db, q)) { sql(db, "ROLLBACK"); return -1; }
  if (sql(db, "COMMIT")) { sql(db, "ROLLBACK"); return -1; }
  return 0;
}

/* a store behind the preallocation layer (store/io_prealloc.c) when pa is not 0: its chunk size in bytes */
static void rmio(fk_io *io) { fk_prealloc_release(io); }
#define CLEANUP_IO __attribute__((cleanup(rmio)))
static fk_io mkio(SimDisk *d, uint64_t pa) {
  fk_io io = sim_io(d);
  if (pa) fk_prealloc_wrap(&io, pa);
  return io;
}

static void workload(SimDisk *d, int batch, uint64_t pa, Ack *a) {
  a->ack0 = a->ack1 = -1;
  a->forked = 0;
  fk_io io CLEANUP_IO = mkio(d, pa);
  fk_store *s = NULL;
  FkVfs *v = NULL;
  sqlite3 *db0 = NULL, *db1 = NULL;
  if (fk_open(&io, &s) != 0) { return; }
  if (fkvfs_register(s, "fkp", &v) != SQLITE_OK) { fk_close(s); return; }
  fkvfs_set_batch_atomic(v, batch);
  db0 = openb("fkp", 0);
  if (!db0) goto out;
  if (sql(db0, "BEGIN; CREATE TABLE kv(id INTEGER PRIMARY KEY, txn INT, tag INT, pad BLOB); CREATE TABLE meta(last INT); INSERT INTO meta VALUES(0); COMMIT")) goto out;
  a->ack0 = 0;
  for (int i = 1; i <= NT; i++) {
    if (do_txn(db0, 0, i)) goto out;
    a->ack0 = i;
    if (i == 3) {
      uint32_t nb = 0;
      if (fk_fork(s, 0, &nb) != FK_OK) goto out;
      a->forked = 1;
      a->ack1 = 3;
      db1 = openb("fkp", nb);
      if (!db1) goto out;
    }
    if (i >= 4 && db1) {
      if (do_txn(db1, 1, i)) goto out;
      a->ack1 = i;
    }
  }
out:
  if (db0) sqlite3_close_v2(db0);
  if (db1) sqlite3_close_v2(db1);
  int rc = fkvfs_unregister(v);
  if (rc != SQLITE_OK) { fails++; printf("  FAIL: workload left a connection open\n"); }
  fk_close(s);
}

typedef struct { Ack a; int batch; uint64_t pa; long images, violations; const char *what; long crash_at; } Ctx;

static long expect_rows(int last) { long n = 0; for (int j = 1; j <= last; j++) n += rows_for(j); return n; }

/* returns 0 if the branch is consistent with "at least lo transactions were acknowledged, at most hi may have landed" */
static int check_branch(sqlite3 *db, int branch, int lo, int hi, char *why, size_t wn) {
  if (scalar(db, "SELECT count(*) FROM sqlite_master") == 0) {
    if (lo >= 0) { snprintf(why, wn, "b%d: empty database but %d transactions were acknowledged", branch, lo); return 1; }
    return 0;
  }
  if (scalar(db, "SELECT count(*) FROM sqlite_master WHERE name IN ('kv','meta')") != 2) { snprintf(why, wn, "b%d: setup transaction applied partially", branch); return 1; }
  long long last = scalar(db, "SELECT last FROM meta");
  if (last < (lo < 0 ? 0 : lo) || last > hi) { snprintf(why, wn, "b%d: last=%lld, acknowledged %d, at most %d", branch, last, lo, hi); return 1; }
  if (scalar(db, "SELECT count(*) FROM kv") != expect_rows((int)last)) { snprintf(why, wn, "b%d: %lld rows, expected %ld for last=%lld", branch, scalar(db, "SELECT count(*) FROM kv"), expect_rows((int)last), last); return 1; }
  if (scalar(db, "SELECT count(*) FROM (SELECT txn FROM kv GROUP BY txn HAVING count(*) != CASE WHEN txn%3=0 THEN 40 ELSE 3 END)") != 0) { snprintf(why, wn, "b%d: a transaction is partly there", branch); return 1; }
  if (scalar(db, "SELECT coalesce(max(txn),0) FROM kv") != last) { snprintf(why, wn, "b%d: meta and rows disagree", branch); return 1; }
  if (scalar(db, branch == 0 ? "SELECT count(*) FROM kv WHERE tag!=0" : "SELECT count(*) FROM kv WHERE tag != (txn>3)") != 0) { snprintf(why, wn, "b%d: rows from the other branch", branch); return 1; }
  if (!integrity_ok(db)) { snprintf(why, wn, "b%d: integrity_check failed", branch); return 1; }
  return 0;
}

static void verify_image(void *cx, const uint8_t *img, uint64_t len) {
  Ctx *c = cx;
  c->images++;
  SimDisk d;
  sim_init(&d, img, len);
  fk_io io CLEANUP_IO = mkio(&d, c->pa);
  fk_store *s = NULL;
  FkVfs *v = NULL;
  char why[200] = "";
  int bad = 0;
  if (fk_open(&io, &s) != 0) { bad = 1; snprintf(why, sizeof why, "the store does not open"); goto done; }
  if (fkvfs_register(s, "fkq", &v) != SQLITE_OK) { bad = 1; snprintf(why, sizeof why, "register"); goto done; }
  fkvfs_set_batch_atomic(v, c->batch);
  sqlite3 *db = openb("fkq", 0);
  if (!db) { bad = 1; snprintf(why, sizeof why, "b0 does not open"); goto closeit; }
  bad = check_branch(db, 0, c->a.ack0, c->a.ack0 < 0 ? 0 : c->a.ack0 + 1, why, sizeof why);
  sqlite3_close(db);
  if (!bad) {
    if (c->a.forked && !fk_branch_alive(s, 1)) { bad = 1; snprintf(why, sizeof why, "an acknowledged fork is gone"); }
    else if (fk_branch_alive(s, 1)) {
      db = openb("fkq", 1);
      if (!db) { bad = 1; snprintf(why, sizeof why, "b1 does not open"); }
      else {
        if (c->a.forked) bad = check_branch(db, 1, c->a.ack1, c->a.ack1 + 1, why, sizeof why);
        else bad = check_branch(db, 1, 3, 3, why, sizeof why); /* the fork may have landed without being acknowledged: then it is b0 as of transaction 3 */
        sqlite3_close(db);
      }
    }
  }
closeit:
  fkvfs_unregister(v);
done:
  if (s) fk_close(s);
  sim_free(&d);
  if (bad) {
    c->violations++;
    if (c->violations <= 5) printf("  VIOLATION (%s, event %ld, batch=%d): %s\n", c->what, c->crash_at, c->batch, why);
  }
}

static void power_loss(int batch, uint64_t pa) {
  printf("T11 power loss under SQLite (batch advertised=%d%s): %d transactions on b0, a fork after the third, %d on b1\n", batch, pa ? ", behind the preallocation layer" : "", NT, NT - 3);
  SimDisk d;
  sim_init(&d, NULL, 0);
  Ack a;
  workload(&d, batch, pa, &a);
  long events = d.events;
  CHECK(a.ack0 == NT && a.ack1 == NT, "the uninterrupted run completes (acks %d, %d)", a.ack0, a.ack1);
  sim_free(&d);
  const char *se = getenv("VFS_STEP");
  long step = se ? atol(se) : 1;
  if (step < 1) step = 1;
  Rng r;
  rng_seed(&r, 12345u + (uint64_t)batch);
  long images = 0, viol = 0, points = 0, dropped_acks = 0;
  for (int mode = 0; mode < 2; mode++) { /* 0: the power is cut before I/O event N; 1: the fsync number N fails, then the power is cut */
    for (long n = (mode ? 1 : 0); n <= events + 1; n += step) {
      SimDisk w;
      sim_init(&w, NULL, 0);
      if (mode == 0) w.crash_at = n; else w.fsync_fail_at = n;
      Ctx c;
      memset(&c, 0, sizeof c);
      workload(&w, batch, pa, &c.a);
      c.batch = batch;
      c.pa = pa;
      c.what = mode ? "fsync fails" : "power cut";
      c.crash_at = n;
      sim_images(&w, &r, verify_image, &c);
      images += c.images;
      viol += c.violations;
      points++;
      if (c.a.ack0 < NT) dropped_acks++;
      sim_free(&w);
    }
  }
  printf("   %ld I/O events in the run; %ld crash or failure points, %ld post-crash images checked, %ld violations\n", events, points, images, viol);
  CHECK(viol == 0, "every post-crash image is a state SQLite could have committed (%ld violations)", viol);
  CHECK(images > 0 && dropped_acks > 0, "the harness actually cut runs short");
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int quick = argc > 1 && strcmp(argv[1], "quick") == 0;
  int only_power = argc > 1 && strcmp(argv[1], "power") == 0;
  snprintf(dir, sizeof dir, "/tmp/fkvfs-test-XXXXXX");
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
  printf("SQLite %s, batch-atomic compiled in: %s\n", sqlite3_libversion(), have_batch() ? "yes" : "no");
  if (!only_power) {
    t_basic(1, 0);
    t_basic(0, 0);
    t_basic(0, 1);
    t_fork();
    t_fallback();
    t_failed_publish();
    t_trunc_rewrite();
    t_spill(1);
    t_spill(0);
    t_pagesize();
    t_modes();
    t_conns(1);
    t_conns(0);
    t_vacuum();
    t_temp();
    t_names();
  }
  if (!quick) {
    if (have_batch()) power_loss(1, 0);
    power_loss(0, 0);
    power_loss(0, 16384); /* the journal path again, behind the preallocation layer with small chunks so that the cuts include the zero fills */
  }
  rmdir(dir);
  printf("checks=%d TOTAL FAILS=%d\n", checks, fails);
  return fails ? 1 : 0;
}
