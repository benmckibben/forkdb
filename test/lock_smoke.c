/* lock_smoke: the single-opener lock survives compaction, and a store keeps its place when the process changes directory. Portable: runs on Linux, macOS and Windows.
 *
 * An independent review of 6f9e979 found that on Linux and macOS a compaction replaced the store with a new file that nobody held a lock on: the first compaction ended the
 * single-opener guarantee, a second process could open the store while the first still wrote to it, and an acknowledged commit of the second was lost. No test combined the lock with
 * a compaction. And a relative path was kept as given, so a process that called chdir() after opening sent a compaction, and every later commit, to the new directory.
 * Run it from a scratch directory: it makes lock_smoke_dir below the current one. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../store/fkstore.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define CHDIR(p) _chdir(p)
#define GETCWD(b, n) _getcwd(b, (int)(n))
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#define CHDIR(p) chdir(p)
#define GETCWD(b, n) getcwd(b, n)
#endif

static int fails, checks;
#define CHECK(c, msg) do { checks++; int ok_ = !!(c); if (!ok_) { fails++; printf("  FAIL %s\n", msg); } } while (0)

enum { NP = 48 };
static void page(uint8_t *p, int v) { for (int i = 0; i < (int)FK_PAGE; i++) p[i] = (uint8_t)(v * 31 + i); }
static int put(fk_store *s, uint32_t b, uint32_t pg, int v, uint32_t size) {
  uint8_t p[FK_PAGE]; page(p, v);
  return fk_txn_begin(s, b) || fk_txn_write(s, pg, p) || fk_txn_commit(s, size);
}
static int has(fk_store *s, uint32_t b, uint32_t pg, int v) { uint8_t a[FK_PAGE], e[FK_PAGE]; page(e, v); return fk_read(s, b, pg, a) == 0 && !memcmp(a, e, FK_PAGE); }
static int open_store(const char *path, fk_io *io, fk_store **s) { return fk_file_open(path, io) == 0 && fk_open(io, s) == 0 && *s; }
static long file_size(const char *path) { FILE *f = fopen(path, "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }

/* Leaves the file mostly garbage: a branch that rewrites every page twice, then discarded. A compaction then really rewrites the file and the file shrinks. */
static int make_garbage(fk_store *s) {
  uint32_t g = 0;
  if (fk_fork(s, 0, &g) != 0) return 0;
  for (int round = 0; round < 2; round++)
    for (uint32_t pg = 0; pg < NP; pg++) if (put(s, g, pg, 900 + round, NP) != 0) return 0;
  return fk_discard(s, g) == 0;
}
static int fill_main(fk_store *s) {
  for (uint32_t pg = 0; pg < NP; pg++) if (put(s, 0, pg, (int)pg, NP) != 0) return 0;
  return 1;
}

/* The tests below hold a store open and look at what a second opener gets. */
static void lock_survives_compaction(void) {
  printf("== the single-opener lock is still held after a compaction\n");
  const char *path = "lock_smoke_dir/lock.fk";
  remove(path);
  fk_io io, io2; fk_store *s = NULL;
  CHECK(open_store(path, &io, &s) && fill_main(s), "open a store and fill main");
  CHECK(fk_file_open(path, &io2) == FK_ELOCKED, "before any compaction a second open is refused with FK_ELOCKED");
  for (int round = 1; round <= 3; round++) {
    char msg[200];
    long before = file_size(path);
    CHECK(make_garbage(s), "a discarded branch leaves garbage in the file");
    long grown = file_size(path);
    CHECK(fk_compact(s) == 0, "compact");
    long after = file_size(path);
    snprintf(msg, sizeof msg, "round %d: the compaction really rewrote the file (it was %ld bytes with garbage, %ld before it, %ld after)", round, grown, before, after);
    CHECK(after < grown, msg);
    snprintf(msg, sizeof msg, "round %d: after the compaction a second open is STILL refused with FK_ELOCKED", round);
    int rc = fk_file_open(path, &io2);
    CHECK(rc == FK_ELOCKED, msg);
    if (rc == 0) fk_file_close(&io2); /* do not leave a stray lock holder behind, whatever the answer */
    CHECK(put(s, 0, 3, 1000 + round, NP) == 0, "a commit after the compaction is acknowledged");
    rc = fk_file_open(path, &io2);
    CHECK(rc == FK_ELOCKED, "and after that commit the second open is still refused");
    if (rc == 0) fk_file_close(&io2);
  }
  long size = file_size(path);
  CHECK(has(s, 0, 3, 1003) && has(s, 0, 7, 7), "what was committed is there");
  fk_close(s); fk_file_close(&io);
  CHECK(file_size(path) == size, "the refused opens did not change the file");
  CHECK(open_store(path, &io, &s) && has(s, 0, 3, 1003) && has(s, 0, 7, 7), "after the holder let go, the next open succeeds and sees the data");
  fk_close(s); fk_file_close(&io);
}

static void store_keeps_its_place_when_the_directory_changes(void) {
  printf("== a store opened by a relative path is not lost when the process changes directory\n");
  char base[1024];
  CHECK(GETCWD(base, sizeof base) != NULL, "the current directory");
  MKDIR("lock_smoke_dir/d1"); MKDIR("lock_smoke_dir/d2");
  remove("lock_smoke_dir/d1/rel.fk"); remove("lock_smoke_dir/d2/rel.fk"); remove("lock_smoke_dir/d1/rel.fk.compact"); remove("lock_smoke_dir/d2/rel.fk.compact");
  fk_io io; fk_store *s = NULL;
  int ok = CHDIR("lock_smoke_dir/d1") == 0;
  CHECK(ok, "change into d1");
  if (!ok) return;
  CHECK(open_store("rel.fk", &io, &s) && fill_main(s) && make_garbage(s), "open rel.fk by a relative path, fill it, leave garbage");
  CHECK(CHDIR(base) == 0 && CHDIR("lock_smoke_dir/d2") == 0, "the process changes directory to d2, with the store still open");
  CHECK(fk_compact(s) == 0, "compact");
  CHECK(put(s, 0, 5, 2001, NP) == 0 && put(s, 0, 6, 2002, NP) == 0, "commits after the compaction are acknowledged");
  CHECK(CHDIR(base) == 0, "back to the base directory");
  CHECK(file_size("lock_smoke_dir/d2/rel.fk") < 0, "no store appeared in d2, where the process is when it compacts");
  CHECK(file_size("lock_smoke_dir/d2/rel.fk.compact") < 0, "and no temporary file");
  CHECK(file_size("lock_smoke_dir/d1/rel.fk.compact") < 0, "the temporary file of d1 is gone");
  fk_io io2;
  int rc = fk_file_open("lock_smoke_dir/d1/rel.fk", &io2);
  CHECK(rc == FK_ELOCKED, "d1/rel.fk is still locked by the process that holds it");
  if (rc == 0) fk_file_close(&io2);
  fk_close(s); fk_file_close(&io);
  CHECK(open_store("lock_smoke_dir/d1/rel.fk", &io, &s) && has(s, 0, 5, 2001) && has(s, 0, 6, 2002) && has(s, 0, 9, 9),
        "reopened from d1: the commits made after the compaction and the data from before are all in the file");
  fk_close(s); fk_file_close(&io);
}

int main(void) {
  MKDIR("lock_smoke_dir");
  lock_survives_compaction();
  store_keeps_its_place_when_the_directory_changes();
  printf("%d checks, TOTAL FAILS=%d\n", checks, fails);
  return fails ? 1 : 0;
}
