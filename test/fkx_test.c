/* fkx tests on real files, with SQLite through the VFS. Built with -DFK_MAX_BRANCHES=16 so the id limit can be reached. See scripts/test-fkx.sh. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../fkx/fkx_internal.h"
#include "../vfs/fkvfs.h"

#ifndef FK_TEST_DATA
#define FK_TEST_DATA "test/data"
#endif
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char dir[] = "/tmp/fkxtestXXXXXX";
static char path[300];
static const char *P(const char *n) { snprintf(path, sizeof path, "%s/%s", dir, n); return path; }

static fkx *open_x(const char *file, unsigned undo_keep, int create) {
  fkx_options o;
  fkx_options_default(&o);
  o.prealloc_chunk = 0;
  o.undo_keep = undo_keep;
  o.create = create;
  fkx *x = NULL;
  int rc = fkx_open(P(file), &o, &x);
  if (rc) { printf("  fkx_open(%s) = %d\n", file, rc); return NULL; }
  return x;
}
static int idof(fkx *x, const char *ref) { fkx_info i; return fkx_info_get(x, ref, &i) ? -1 : (int)i.id; }
/* page 0 and 1 hold the value v (every byte) */
static int put(fkx *x, const char *ref, int v) {
  int id = idof(x, ref);
  uint8_t pg[FK_PAGE];
  memset(pg, v, sizeof pg);
  fk_store *s = fkx_store_rw(x);
  if (id < 0 || fk_txn_begin(s, (uint32_t)id) || fk_txn_write(s, 0, pg) || fk_txn_write(s, 1, pg)) return -1;
  return fk_txn_commit(s, 2);
}
static int val(fkx *x, const char *ref) {
  int id = idof(x, ref);
  uint8_t pg[FK_PAGE];
  if (id < 0 || fk_read(fkx_store_rw(x), (uint32_t)id, 0, pg)) return -1;
  return pg[0];
}
static size_t nlist(fkx *x, int internal) { size_t n = 0; fkx_list(x, internal, NULL, 0, &n); return n; }
static int nundo(fkx *x, const char *ref) { fkx_info i; return fkx_info_get(x, ref, &i) ? -1 : (int)i.n_undo; }
static int consistent(fkx *x) { char why[200]; int rc = fkx_check(x, why, sizeof why); if (rc) printf("  inconsistent: %s\n", why); return rc == 0; }

static uint64_t seq_vals[16];
static int seq_n, seq_i;
static uint64_t seq_next(void *c) { (void)c; return seq_i < seq_n ? seq_vals[seq_i++] : 1000 + (uint64_t)seq_i++; }

static void t_basic(void) {
  printf("T1 a new repository, names, fork, snapshot\n");
  fkx *x = open_x("t1.fk", 8, 1);
  CHECK(x != NULL, "open");
  if (!x) return;
  fkx_info i;
  CHECK(fkx_info_get(x, "main", &i) == 0 && i.kind == FKX_ROOT && !i.read_only && i.id == 0 && i.uid != 0 && i.n_undo == 0 && strcmp(i.name, "main") == 0, "the root is branch 0, named main");
  uint64_t root_uid = i.uid;
  CHECK(nlist(x, 0) == 1 && nlist(x, 1) == 1, "one branch");
  CHECK(fkx_fork(x, "main", "") == FKX_EINVAL && fkx_fork(x, "main", "#x") == FKX_EINVAL && fkx_fork(x, "main", "a b") == FKX_EINVAL && fkx_fork(x, "main", "x'") == FKX_EINVAL, "bad names are refused");
  char longname[80];
  memset(longname, 'a', 64); longname[64] = 0;
  CHECK(fkx_fork(x, "main", longname) == FKX_EINVAL, "a name of 64 bytes is refused");
  memset(longname, 'a', 63); longname[63] = 0;
  CHECK(fkx_fork(x, "main", longname) == FKX_OK && fkx_discard(x, longname) == FKX_OK, "a name of 63 bytes is fine");
  CHECK(put(x, "main", 11) == 0, "main holds 11");
  CHECK(fkx_fork(x, "nope", "a") == FKX_ENOENT && strstr(fkx_errmsg(x), "nope"), "a source that is not there");
  CHECK(fkx_fork(x, "main", "agent-1") == FKX_OK, "fork");
  CHECK(fkx_fork(x, "main", "agent-1") == FKX_EEXIST && fkx_snapshot(x, "main", "agent-1") == FKX_EEXIST && fkx_fork(x, "main", "main") == FKX_EEXIST, "names are unique");
  CHECK(nlist(x, 0) == 2 && nlist(x, 1) == 3, "a fork is two store branches: the fork and its base");
  fkx_info f, m;
  CHECK(fkx_info_get(x, "agent-1", &f) == 0 && f.kind == FKX_FORK && !f.read_only && f.origin_uid == root_uid && f.base_uid != 0 && f.n_undo == 0, "the fork knows its origin and its base");
  char ref[24];
  snprintf(ref, sizeof ref, "#%016llx", (unsigned long long)f.base_uid);
  CHECK(fkx_info_get(x, ref, &m) == 0 && m.kind == FKX_BASE && m.read_only && m.owner_uid == f.uid && m.name[0] == 0, "the base is found by uid: read-only, owned by the fork, no name");
  CHECK(val(x, "agent-1") == 11 && val(x, ref) == 11, "the fork and its base hold what main held");
  CHECK(put(x, "agent-1", 22) == 0 && val(x, "main") == 11 && val(x, "agent-1") == 22 && val(x, ref) == 11, "writing the fork changes neither main nor the base");
  fkx_info mm, ff;
  fkx_info_get(x, "main", &mm);
  fkx_info_get(x, "agent-1", &ff);
  CHECK(mm.state != ff.state, "different content, different state ids");
  CHECK(fkx_snapshot(x, "agent-1", "snap-1") == FKX_OK && val(x, "snap-1") == 22, "a snapshot");
  fkx_info s;
  CHECK(fkx_info_get(x, "snap-1", &s) == 0 && s.kind == FKX_SNAPSHOT && s.read_only && s.state == ff.state && s.origin_uid == f.uid, "it is read-only, has the state id of its source, and knows where it came from");
  CHECK(nlist(x, 0) == 3 && nlist(x, 1) == 4, "it is one store branch");
  CHECK(fkx_fork(x, "snap-1", "from-snap") == FKX_OK && val(x, "from-snap") == 22, "a fork can start from a snapshot");
  CHECK(fkx_info_get(x, "#zz", &i) == FKX_EINVAL && fkx_info_get(x, "#0000000000000001", &i) == FKX_ENOENT, "bad and unknown uid refs");
  CHECK(consistent(x), "the repository is consistent");
  fkx_close(x);
  x = open_x("t1.fk", 8, 0);
  CHECK(x && nlist(x, 0) == 4 && val(x, "agent-1") == 22 && val(x, "snap-1") == 22 && val(x, "main") == 11 && consistent(x), "reopen: everything is there");
  CHECK(x && fkx_info_get(x, "main", &i) == 0 && i.uid == root_uid, "and the root has the same uid");
  fkx_close(x);
}

static void t_discard_reset(void) {
  printf("T2 discard and reset\n");
  fkx *x = open_x("t2.fk", 8, 1);
  if (!x) { CHECK(0, "open"); return; }
  put(x, "main", 1);
  fkx_fork(x, "main", "a");
  put(x, "a", 2);
  fkx_snapshot(x, "a", "s");
  put(x, "a", 3);
  CHECK(fkx_discard(x, "main") == FKX_EINVAL, "the root cannot be discarded");
  CHECK(fkx_reset(x, "main", NULL) == FKX_EINVAL, "the root has no base to reset to");
  CHECK(fkx_reset(x, "a", "a") == FKX_EINVAL, "reset to itself");
  CHECK(fkx_reset(x, "s", "main") == FKX_EREADONLY, "a snapshot cannot be reset");
  CHECK(fkx_reset(x, "a", "nope") == FKX_ENOENT, "reset to a branch that is not there");
  CHECK(fkx_reset(x, "a", "s") == FKX_OK && val(x, "a") == 2, "reset to a snapshot");
  CHECK(put(x, "a", 4) == 0 && fkx_reset(x, "a", NULL) == FKX_OK && val(x, "a") == 1, "reset to its base throws the work away");
  fkx_info a, mm;
  fkx_info_get(x, "a", &a);
  fkx_info_get(x, "main", &mm);
  CHECK(a.state == mm.state, "and has main's state id again");
  CHECK(fkx_reset(x, "main", "s") == FKX_OK && val(x, "main") == 2, "the root can be reset to a snapshot");
  CHECK(nlist(x, 1) == 4, "(main, a, a's base, s)");
  CHECK(fkx_discard(x, "a") == FKX_OK && nlist(x, 1) == 2 && fkx_info_get(x, "a", &a) == FKX_ENOENT, "discarding a fork discards its base too");
  CHECK(fkx_discard(x, "s") == FKX_OK && nlist(x, 1) == 1, "and a snapshot is one branch");
  CHECK(fkx_discard(x, "s") == FKX_ENOENT, "twice");
  fkx_fork(x, "main", "b");
  fkx_info b;
  fkx_info_get(x, "b", &b);
  char ref[24];
  snprintf(ref, sizeof ref, "#%016llx", (unsigned long long)b.base_uid);
  CHECK(fkx_discard(x, ref) == FKX_EINVAL, "an internal branch is not discarded by hand");
  CHECK(fkx_reset(x, ref, "main") == FKX_EREADONLY, "nor reset");
  CHECK(consistent(x), "consistent");
  fkx_close(x);
}

static void t_promote(void) {
  printf("T3 promote (fast-forward) and undo\n");
  fkx *x = open_x("t3.fk", 8, 1);
  if (!x) { CHECK(0, "open"); return; }
  put(x, "main", 10);
  fkx_fork(x, "main", "w");
  put(x, "w", 20);
  fkx_promote_result r;
  fkx_promote_opts dry = {NULL, 1};
  CHECK(fkx_promote(x, "w", &dry, &r) == FKX_OK && r.method == FKX_METHOD_FAST_FORWARD && val(x, "main") == 10 && nundo(x, "main") == 0, "a dry run says fast-forward and changes nothing");
  CHECK(fkx_promote(x, "w", NULL, &r) == FKX_OK && r.method == FKX_METHOD_FAST_FORWARD && r.undo_recorded, "promote");
  fkx_info mm, w;
  fkx_info_get(x, "main", &mm);
  fkx_info_get(x, "w", &w);
  CHECK(val(x, "main") == 20 && mm.state == w.state && mm.n_undo == 1, "main holds the fork's content and its state id, and has one undo point");
  CHECK(val(x, "w") == 20 && nlist(x, 1) == 5, "the fork is unchanged (main, w, w's base, two undo snapshots)");
  CHECK(fkx_promote(x, "w", NULL, &r) == FKX_OK && r.method == FKX_METHOD_UP_TO_DATE && nundo(x, "main") == 1, "promoting again: up to date, nothing recorded");
  CHECK(consistent(x), "consistent");
  CHECK(fkx_undo(x, "main", &r) == FKX_OK && val(x, "main") == 10 && nundo(x, "main") == 0 && nlist(x, 1) == 3, "undo restores main exactly and removes the undo snapshots");
  fkx_info_get(x, "main", &mm);
  fkx_info base;
  char ref[24];
  snprintf(ref, sizeof ref, "#%016llx", (unsigned long long)w.base_uid);
  fkx_info_get(x, ref, &base);
  CHECK(mm.state == base.state, "main has its old state id back, so w is again a fork of where main is");
  CHECK(fkx_undo(x, "main", &r) == FKX_ENOENT, "nothing more to undo");
  CHECK(fkx_promote(x, "w", NULL, &r) == FKX_OK && r.method == FKX_METHOD_FAST_FORWARD && val(x, "main") == 20, "and it can be promoted again");

  /* main moves: no fast-forward, and no undo that would lose its work */
  fkx_undo(x, "main", NULL);
  put(x, "main", 30);
  CHECK(fkx_promote(x, "w", NULL, &r) == FKX_ENOSESSION && r.method == FKX_METHOD_REPLAY && val(x, "main") == 30 && nundo(x, "main") == 0 && nlist(x, 1) == 3, "main has moved: promote needs a replay, which this build cannot do; nothing changed");
  fkx_discard(x, "w");
  fkx_fork(x, "main", "w2");
  put(x, "w2", 40);
  CHECK(fkx_promote(x, "w2", NULL, &r) == FKX_OK && val(x, "main") == 40, "a fork of the new main is promoted");
  put(x, "main", 50);
  CHECK(fkx_undo(x, "main", &r) == FKX_ENOSESSION && val(x, "main") == 50 && nundo(x, "main") == 1, "main changed after the promote: undo would lose that work, refused, nothing changed");

  /* an undo that was cut after the reset: the entry is still there, main already holds the earlier state */
  put(x, "main", 40 /* back to the promote's content for the test */);
  fkx_discard(x, "w2");
  fkx_info lst[16];
  size_t n = 0;
  fkx_list(x, 1, lst, 16, &n);
  uint32_t before_id = 0;
  fkx_info mi;
  fkx_info_get(x, "main", &mi);
  (void)mi;
  for (size_t k = 0; k < n; k++) if (lst[k].kind == FKX_UNDO) { before_id = lst[k].id; break; }
  (void)before_id;
  CHECK(consistent(x), "consistent");
  fkx_close(x);

  x = open_x("t3b.fk", 8, 1);
  put(x, "main", 1);
  fkx_fork(x, "main", "f");
  put(x, "f", 2);
  fkx_promote(x, "f", NULL, NULL);
  fkx_list(x, 1, lst, 16, &n);
  uint32_t undo_before = 0;
  uint64_t main_uid = 0;
  fkx_info_get(x, "main", &mi);
  main_uid = mi.uid;
  for (size_t k = 0; k < n; k++) if (lst[k].kind == FKX_UNDO && lst[k].owner_uid == main_uid) { char rr[24]; snprintf(rr, sizeof rr, "#%016llx", (unsigned long long)lst[k].uid); if (val(x, rr) == 1) undo_before = lst[k].id; }
  CHECK(undo_before != 0, "found the before snapshot");
  CHECK(fk_reset(fkx_store_rw(x), 0, undo_before) == 0 && val(x, "main") == 1, "(simulate: the reset of an undo was done, and a power cut came before the entry was removed)");
  CHECK(fkx_undo(x, "main", NULL) == FKX_OK && val(x, "main") == 1 && nundo(x, "main") == 0 && nlist(x, 1) == 3 && consistent(x), "undo finds main already restored and only removes the entry");
  fkx_close(x);

  /* retention */
  x = open_x("t3c.fk", 2, 1);
  put(x, "main", 100);
  char nm[16];
  for (int k = 1; k <= 4; k++) {
    snprintf(nm, sizeof nm, "f%d", k);
    fkx_fork(x, "main", nm);
    put(x, nm, 100 + k);
    CHECK(fkx_promote(x, nm, NULL, NULL) == FKX_OK && val(x, "main") == 100 + k, "promote %d", k);
    fkx_discard(x, nm);
  }
  CHECK(nundo(x, "main") == 2 && nlist(x, 1) == 5, "with a retention of 2: two undo points (four snapshots) and main");
  CHECK(fkx_undo(x, "main", NULL) == FKX_OK && val(x, "main") == 103 && fkx_undo(x, "main", NULL) == FKX_OK && val(x, "main") == 102 && fkx_undo(x, "main", NULL) == FKX_ENOENT, "undo goes back two promotes and no further");
  CHECK(nlist(x, 1) == 1 && consistent(x), "and leaves no snapshots");
  fkx_close(x);
  x = open_x("t3d.fk", 0, 1);
  put(x, "main", 1);
  fkx_fork(x, "main", "f");
  put(x, "f", 2);
  fkx_promote_result pr;
  CHECK(fkx_promote(x, "f", NULL, &pr) == FKX_OK && val(x, "main") == 2 && !pr.undo_recorded && nlist(x, 1) == 3, "with a retention of 0 a promote records nothing");
  fkx_close(x);
}

static void t_open_connections(void) {
  printf("T4 open connections, SQLite through the VFS, read-only branches\n");
  fkx *x = open_x("t4.fk", 8, 1);
  if (!x) { CHECK(0, "open"); return; }
  sqlite3 *db = NULL, *db2 = NULL;
  CHECK(fkx_open_db(x, "main", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, &db) == 0, "open main");
  CHECK(sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t VALUES(1,'a'),(2,'b')", 0, 0, 0) == 0, "a table");
  fkx_info i;
  CHECK(fkx_info_get(x, "main", &i) == 0 && i.open_connections == 1, "the connection is counted");
  CHECK(fkx_snapshot(x, "main", "s0") == 0 && fkx_fork(x, "main", "w") == 0, "snapshot and fork while a connection is open (reads are unaffected)");
  CHECK(fkx_reset(x, "main", "s0") == FKX_EBUSY, "reset of a branch with a connection open is refused");
  sqlite3_close(db);
  CHECK(fkx_open_db(x, "w", SQLITE_OPEN_READWRITE, &db) == 0 && sqlite3_exec(db, "INSERT INTO t VALUES(3,'c')", 0, 0, 0) == 0, "write the fork through SQLite");
  CHECK(fkx_discard(x, "w") == FKX_EBUSY && fkx_promote(x, "w", NULL, NULL) == FKX_OK, "a connection on the fork blocks its discard, not its promote");
  CHECK(fkx_info_get(x, "main", &i) == 0 && i.n_undo == 1, "promoted");
  sqlite3_close(db);
  CHECK(fkx_open_db(x, "main", SQLITE_OPEN_READONLY, &db) == 0 && sqlite3_exec(db, "SELECT 1 FROM t WHERE id=3", 0, 0, 0) == 0, "main has the fork's row");
  sqlite3_stmt *st;
  sqlite3_prepare_v2(db, "SELECT count(*) FROM t", -1, &st, 0);
  int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
  sqlite3_finalize(st);
  CHECK(n == 3, "three rows (%d)", n);
  CHECK(fkx_promote(x, "w", NULL, NULL) == FKX_OK, "(already up to date)");
  fkx_fork(x, "main", "w3");
  CHECK(fkx_open_db(x, "w3", SQLITE_OPEN_READWRITE, &db2) == 0 && sqlite3_exec(db2, "INSERT INTO t VALUES(4,'d')", 0, 0, 0) == 0, "another fork with work");
  sqlite3_close(db2);
  CHECK(fkx_promote(x, "w3", NULL, NULL) == FKX_EBUSY && nundo(x, "main") == 1, "main is open: a fast-forward is refused, nothing recorded");
  CHECK(fkx_undo(x, "main", NULL) == FKX_EBUSY, "so is an undo");
  sqlite3_close(db);
  CHECK(fkx_promote(x, "w3", NULL, NULL) == FKX_OK && nundo(x, "main") == 2, "closed: it goes through");
  /* read-only branches */
  CHECK(fkx_open_db(x, "s0", SQLITE_OPEN_READWRITE, &db) == FKX_EREADONLY && fkx_open_db(x, "s0", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, &db) == FKX_EREADONLY, "a snapshot cannot be opened for writing through fkx");
  CHECK(fkx_open_db(x, "s0", SQLITE_OPEN_READONLY, &db) == 0 && sqlite3_exec(db, "SELECT count(*) FROM t", 0, 0, 0) == 0, "but can be read");
  sqlite3_close(db);
  fkx_info s0;
  fkx_info_get(x, "s0", &s0);
  char bn[24];
  fkvfs_branch_name(s0.id, bn, sizeof bn);
  sqlite3 *raw = NULL;
  int rc = sqlite3_open_v2(bn, &raw, SQLITE_OPEN_READWRITE, fkx_vfs_name(x));
  CHECK(rc != SQLITE_OK, "opening it read-write straight through the VFS is refused too (rc=%d)", rc);
  if (raw) sqlite3_close(raw);
  raw = NULL;
  CHECK(sqlite3_open_v2(bn, &raw, SQLITE_OPEN_READONLY, fkx_vfs_name(x)) == SQLITE_OK && sqlite3_exec(raw, "SELECT count(*) FROM t", 0, 0, 0) == 0, "and read-only it works");
  if (raw) sqlite3_close(raw);
  fkx_fork(x, "main", "w4");
  fkx_info w4;
  fkx_info_get(x, "w4", &w4);
  fkvfs_branch_name(w4.id, bn, sizeof bn);
  raw = NULL;
  CHECK(sqlite3_open_v2(bn, &raw, SQLITE_OPEN_READWRITE, fkx_vfs_name(x)) == SQLITE_OK, "a fork can be opened for writing directly");
  if (raw) sqlite3_close(raw);
  CHECK(fkx_open_db(x, "nope", SQLITE_OPEN_READONLY, &db) == FKX_ENOENT, "no such branch");
  CHECK(consistent(x), "consistent");
  fkx_close(x);
}

static void t_gc_stores(void) {
  printf("T5 garbage collection, foreign stores, versions, limits, ids\n");
  fkx *x = open_x("t5.fk", 8, 1);
  if (!x) { CHECK(0, "open"); return; }
  put(x, "main", 1);
  fkx_fork(x, "main", "a");
  fkx_fork(x, "main", "b");
  fkx_gc_report g;
  CHECK(fkx_gc(x, &g) == 0 && g.orphan_bases == 0 && g.orphan_undo == 0 && g.unlisted_undo == 0 && nlist(x, 1) == 5, "nothing to collect in a healthy repository");
  /* an orphan base: its fork was discarded behind fkx's back (a cut in the middle of fkx_discard leaves exactly this) */
  fkx_info a;
  fkx_info_get(x, "a", &a);
  CHECK(fk_discard(fkx_store_rw(x), a.id) == 0 && nlist(x, 1) == 4, "(a's fork is gone, its base is left)");
  CHECK(fkx_gc(x, &g) == 0 && g.orphan_bases == 1 && nlist(x, 1) == 3 && consistent(x), "gc removes the orphan base");
  /* undo points: listed ones are kept, unlisted ones go */
  put(x, "b", 2);
  fkx_promote(x, "b", NULL, NULL);
  CHECK(nlist(x, 1) == 5 && fkx_gc(x, &g) == 0 && g.unlisted_undo == 0 && nlist(x, 1) == 5 && nundo(x, "main") == 1, "an undo point the owner lists is kept");
  uint8_t mb[FK_META_MAX];
  size_t ml = 0;
  CHECK(fk_meta_get(fkx_store_rw(x), 0, mb, sizeof mb, &ml) == 0 && ml == 40 + 4 + 16, "(main's metadata: 40 + name + one undo entry)");
  mb[7] = 0; /* no undo entries */
  CHECK(fk_meta_set(fkx_store_rw(x), 0, mb, 44) == 0 && nundo(x, "main") == 0, "(main stops listing it: what a cut between taking the snapshots and listing them leaves)");
  CHECK(fkx_gc(x, &g) == 0 && g.unlisted_undo == 2 && nlist(x, 1) == 3 && consistent(x), "gc removes the two unlisted undo snapshots");
  fkx_close(x);

  /* a store that was not made by fkx */
  remove(P("raw.fk"));
  fk_io io;
  fk_store *s = NULL;
  uint32_t f;
  CHECK(fk_file_open(P("raw.fk"), &io) == 0 && fk_open(&io, &s) == 0 && fk_fork(s, 0, &f) == 0, "a raw store with a fork");
  fk_close(s);
  fk_file_close(&io);
  fkx_options o;
  fkx_options_default(&o);
  o.prealloc_chunk = 0;
  fkx *y = NULL;
  CHECK(fkx_open(P("raw.fk"), &o, &y) == FKX_ECORRUPT && y == NULL, "it is refused");
  remove(P("raw2.fk"));
  uint8_t pg[FK_PAGE];
  memset(pg, 9, sizeof pg);
  CHECK(fk_file_open(P("raw2.fk"), &io) == 0 && fk_open(&io, &s) == 0 && fk_txn_begin(s, 0) == 0 && fk_txn_write(s, 0, pg) == 0 && fk_txn_commit(s, 1) == 0, "a raw store with only branch 0");
  fk_close(s);
  fk_file_close(&io);
  CHECK(fkx_open(P("raw2.fk"), &o, &y) == 0 && y && val(y, "main") == 9, "is adopted, with its content");
  if (y) fkx_close(y);
  /* versions */
  char src[400];
  snprintf(src, sizeof src, "%s/v3-compacted.fk", FK_TEST_DATA);
  {
    char cmd[900];
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", src, P("v3.fk"));
    CHECK(system(cmd) == 0, "(a copy of the version 3 fixture)");
  }
  y = NULL;
  CHECK(fkx_open(P("v3.fk"), &o, &y) == FKX_EVERSION && y == NULL, "a version 3 store is refused without upgrade");
  o.upgrade = 1;
  CHECK(fkx_open(P("v3.fk"), &o, &y) == FKX_ECORRUPT && y == NULL, "with upgrade it is compacted and then found to hold branches fkx did not make");
  o.upgrade = 0;
  CHECK(fkx_open(P("none.fk"), &(fkx_options){.create = 0, .vfs_name = "forkdb", .root_name = "main"}, &y) == FKX_ENOENT, "create=0 on a file that is not there");
  /* ids */
  seq_n = 0; seq_i = 0;
  o.random64 = seq_next;
  o.undo_keep = 8;
  seq_vals[0] = 7; seq_vals[1] = 7; seq_vals[2] = 7; seq_vals[3] = 8; seq_vals[4] = 9; seq_n = 5;
  CHECK(fkx_open(P("ids.fk"), &o, &y) == 0, "open with an injected uid source");
  fkx_info i;
  CHECK(fkx_info_get(y, "main", &i) == 0 && i.uid == 7, "the root drew 7");
  CHECK(fkx_fork(y, "main", "f") == 0, "fork");
  fkx_info fi;
  CHECK(fkx_info_get(y, "f", &fi) == 0 && fi.base_uid == 8 && fi.uid == 9, "a draw that collides with a live uid is drawn again (7, 7 skipped): base 8, fork 9");
  fkx_close(y);
  /* the id limit: FK_MAX_BRANCHES is 16 in this build */
  o.random64 = NULL;
  CHECK(fkx_open(P("lim.fk"), &o, &y) == 0, "open");
  int made = 0;
  char nm[16];
  for (int k = 0; k < 20; k++) { snprintf(nm, sizeof nm, "f%d", k); int rc = fkx_fork(y, "main", nm); if (rc == FKX_ELIMIT) break; if (rc) break; made++; }
  CHECK(made == 7 && nlist(y, 1) == 15, "seven forks (two store branches each) and the root fit in 16 (made %d, %zu)", made, nlist(y, 1));
  CHECK(fkx_snapshot(y, "main", "s") == 0 && fkx_snapshot(y, "main", "s2") == FKX_ELIMIT && strstr(fkx_errmsg(y), "16"), "a snapshot takes the last one, and the next is refused with the numbers");
  CHECK(fkx_discard(y, "s") == 0 && fkx_discard(y, "f3") == 0 && fkx_fork(y, "main", "again") == 0, "discarding frees ids");
  int did = -1;
  CHECK(fkx_compact_if_needed(y, 1u << 30, &did) == 0 && did == 0, "compaction only when needed");
  fkx_close(y);
}

int main(void) {
  if (!mkdtemp(dir)) return 2;
  t_basic();
  t_discard_reset();
  t_promote();
  t_open_connections();
  t_gc_stores();
  char cmd[400];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  if (system(cmd)) {}
  printf("%d checks, TOTAL FAILS=%d\n", checks, fails);
  return fails ? 1 : 0;
}
