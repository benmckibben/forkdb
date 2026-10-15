/* compact_smoke: a compaction that really compacts, on every platform the real-file backend runs on.
 *
 * Builds a store with a main branch, forks that diverge (one of them forked from a fork), and a large branch that is rewritten and discarded, so that most of the
 * file is garbage. Then compacts, and checks: it did compact (return code and *did), the file is exactly the size of the live data and under half of what it was,
 * every live branch reads byte for byte what it read before (a digest of every page), the discarded branch is gone, a second compaction does nothing, no
 * temporary file is left, writes and forks work afterwards, and after closing and reopening everything still reads the same.
 * It runs four ways: durable and FK_FILE_NOSYNC, each with and without the preallocation layer (the loadable extension uses the layer).
 *
 * Windows build (see .github/workflows/ci.yml): the replacement of the log by the compacted file is MoveFileExW over a file this process holds open, which only
 * works with the right share mode; win_smoke.c never compacted. With -DFK_COUNT_FLUSH and test/win_flush_redirect.h force-included into io_file_win.c the test also counts
 * FlushFileBuffers: more than 0 for a durable session through compaction, exactly 0 for a nosync one.
 *
 * Build (POSIX): gcc -Wall -Wextra -std=gnu11 -O2 test/compact_smoke.c store/fkstore.c store/io_file.c store/io_prealloc.c -o compact_smoke
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../store/fkstore.h"

#ifdef FK_COUNT_FLUSH
#include <windows.h>
static long flushes;
int fk_test_flush(HANDLE h) { flushes++; return FlushFileBuffers(h); } /* the redirect header points io_file_win.c here; this file does not redirect */
#endif

static int fails, checks;
#define CHECK(c, msg) do { checks++; int ok_ = !!(c); if (!ok_) { fails++; printf("  FAIL %s\n", msg); } } while (0)

#define NP 96u /* pages in main */

static void page(uint8_t *p, int v) { for (int i = 0; i < (int)FK_PAGE; i++) p[i] = (uint8_t)(v * 31 + i); }
static int put(fk_store *s, uint32_t b, uint32_t pg, int v, uint32_t size) {
  uint8_t p[FK_PAGE];
  page(p, v);
  return fk_txn_begin(s, b) || fk_txn_write(s, pg, p) || fk_txn_commit(s, size);
}
/* rewrite every page of a branch, ten pages to a commit */
static int rewrite(fk_store *s, uint32_t b, int v, uint32_t pages) {
  for (uint32_t base = 0; base < pages; base += 10) {
    uint8_t p[FK_PAGE];
    if (fk_txn_begin(s, b)) return -1;
    for (uint32_t pg = base; pg < base + 10 && pg < pages; pg++) { page(p, v + (int)pg); if (fk_txn_write(s, pg, p)) { fk_txn_abort(s); return -1; } }
    if (fk_txn_commit(s, pages)) return -1;
  }
  return 0;
}
/* FNV-1a over the size and every page of a branch: equal digests mean the same content */
static uint64_t digest(fk_store *s, uint32_t b) {
  uint64_t h = 14695981039346656037ull;
  uint32_t n = 0;
  uint8_t p[FK_PAGE];
  if (fk_size(s, b, &n)) return 0;
  h = (h ^ n) * 1099511628211ull;
  for (uint32_t pg = 0; pg < n; pg++) {
    if (fk_read(s, b, pg, p)) return 0;
    for (size_t i = 0; i < FK_PAGE; i++) h = (h ^ p[i]) * 1099511628211ull;
  }
  return h ? h : 1;
}
static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

#define NB 5
static const char *const bname[NB] = {"main", "a", "b", "c", "d (forked from a)"};

static void session(const char *path, int flags, int prealloc) {
  char tmp[512], msg[160];
  snprintf(tmp, sizeof tmp, "%s.compact", path);
  remove(path);
  remove(tmp);

  fk_io io;
  fk_store *s = NULL;
  CHECK(fk_file_open_ex(path, &io, flags) == 0, "open the file");
  if (prealloc) CHECK(fk_prealloc_wrap(&io, FK_PREALLOC_DEFAULT) == 0, "preallocation layer");
  CHECK(fk_open(&io, &s) == 0 && s, "open the store");
  if (!s) return;

  /* main, then forks that diverge */
  CHECK(rewrite(s, 0, 1, NP) == 0, "write main");
  uint32_t id[NB] = {0}, g = 0;
  CHECK(fk_fork(s, 0, &id[1]) == 0 && fk_fork(s, 0, &id[2]) == 0 && fk_fork(s, 0, &id[3]) == 0, "fork a, b, c");
  CHECK(put(s, id[1], 0, 1001, NP) == 0 && put(s, id[1], 7, 1002, NP) == 0, "a diverges");
  CHECK(put(s, id[2], 20, 2001, NP) == 0 && put(s, id[2], NP, 2002, NP + 1) == 0, "b diverges and grows");
  CHECK(put(s, id[3], 5, 3001, NP - 10) == 0, "c shrinks");
  CHECK(put(s, id[3], NP - 1, 3002, NP) == 0, "and grows back: the pages in between are zeros, not main's data (a tombstone the compaction must keep)");
  { uint8_t z[FK_PAGE], got[FK_PAGE]; memset(z, 0, sizeof z); CHECK(fk_read(s, id[3], NP - 5, got) == 0 && !memcmp(got, z, FK_PAGE), "c reads zeros where main has data, before the compaction"); }
  CHECK(fk_fork(s, id[1], &id[4]) == 0 && put(s, id[4], 1, 4001, NP) == 0, "d forks from a and diverges");
  /* the garbage: a branch rewritten twice over and discarded */
  CHECK(fk_fork(s, 0, &g) == 0 && rewrite(s, g, 5000, NP) == 0 && rewrite(s, g, 9000, NP) == 0, "a big branch is rewritten twice");
  CHECK(fk_discard(s, g) == 0 && !fk_branch_alive(s, g), "and discarded");

  uint64_t before[NB], file0 = 0, live0 = 0;
  for (int i = 0; i < NB; i++) { before[i] = digest(s, i ? id[i] : 0); CHECK(before[i] != 0, "read a branch before the compaction"); }
  CHECK(fk_stats(s, &file0, &live0) == 0 && file0 >= 2 * live0 && live0 > 0, "most of the file is garbage before the compaction");

  int did = 0;
  int rc = fk_compact_if_needed(s, 0, &did);
  snprintf(msg, sizeof msg, "compaction ran and succeeded (rc=%d, did=%d)", rc, did);
  CHECK(rc == 0 && did == 1, msg);
  if (rc != 0) { /* the store is untouched and usable after an error before the replacement: carry on, so the failure shows what else is wrong */ }

  uint64_t file1 = 0, live1 = 0;
  CHECK(fk_stats(s, &file1, &live1) == 0, "stats after");
  snprintf(msg, sizeof msg, "the file is exactly the live data and under half of what it was (before %llu, live %llu, after %llu)", (unsigned long long)file0, (unsigned long long)live0, (unsigned long long)file1);
  CHECK(file1 == live0 && file1 * 2 < file0, msg);
  CHECK(!file_exists(tmp), "no temporary file is left");
  CHECK(!fk_branch_alive(s, g), "the discarded branch stays gone");
  for (int i = 0; i < NB; i++) {
    snprintf(msg, sizeof msg, "branch %s reads exactly what it read before the compaction", bname[i]);
    CHECK(digest(s, i ? id[i] : 0) == before[i], msg);
  }
  did = -1;
  CHECK(fk_compact_if_needed(s, 0, &did) == 0 && did == 0, "a second compaction finds nothing to do");

  /* the store is usable afterwards */
  CHECK(put(s, id[1], 3, 6001, NP) == 0, "commit after the compaction");
  uint8_t pg[FK_PAGE], want[FK_PAGE];
  page(want, 6001);
  CHECK(fk_read(s, id[1], 3, pg) == 0 && !memcmp(pg, want, FK_PAGE), "and read it back");
  uint32_t e = 0;
  CHECK(fk_fork(s, 0, &e) == 0 && digest(s, e) == before[0], "a fork after the compaction holds what main holds");
  uint64_t after[NB];
  for (int i = 0; i < NB; i++) after[i] = digest(s, i ? id[i] : 0);
  CHECK(after[0] == before[0] && after[1] != before[1] && after[2] == before[2], "only the branch written after the compaction changed");
  { uint8_t z[FK_PAGE], got[FK_PAGE]; memset(z, 0, sizeof z); CHECK(fk_read(s, id[3], NP - 5, got) == 0 && !memcmp(got, z, FK_PAGE), "after the compaction c still reads zeros where main has data"); }

  fk_close(s);
  if (prealloc) fk_prealloc_close(&io);
  fk_file_close(&io);

  /* reopen: durable, whatever the session used */
  CHECK(fk_file_open(path, &io) == 0 && fk_open(&io, &s) == 0 && s, "reopen the file and the store");
  if (s) {
    for (int i = 0; i < NB; i++) {
      snprintf(msg, sizeof msg, "after reopening, branch %s reads what it read", bname[i]);
      CHECK(digest(s, i ? id[i] : 0) == after[i], msg);
    }
    CHECK(fk_branch_alive(s, e) && digest(s, e) == before[0] && !fk_branch_alive(s, g), "the fork made after the compaction survives, the discarded branch is still gone");
    fk_close(s);
  }
  fk_file_close(&io);
  remove(path);
  remove(tmp);
}

int main(void) {
  static const struct { const char *path; int flags, prealloc; const char *what; } v[] = {
    {"compact_smoke_a.fk", 0, 0, "durable"},
    {"compact_smoke_b.fk", 0, 1, "durable, preallocation layer"},
    {"compact_smoke_c.fk", FK_FILE_NOSYNC, 0, "nosync"},
    {"compact_smoke_d.fk", FK_FILE_NOSYNC, 1, "nosync, preallocation layer"},
  };
  for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
    printf("== %s\n", v[i].what);
    int f0 = fails;
#ifdef FK_COUNT_FLUSH
    long fl0 = flushes;
#endif
    session(v[i].path, v[i].flags, v[i].prealloc);
#ifdef FK_COUNT_FLUSH
    long used = flushes - fl0;
    char msg[120];
    if (v[i].flags & FK_FILE_NOSYNC) {
      snprintf(msg, sizeof msg, "no FlushFileBuffers in a nosync session through compaction (%ld)", used);
      CHECK(used == 0, msg);
    } else {
      snprintf(msg, sizeof msg, "FlushFileBuffers was called in a durable session (%ld)", used);
      CHECK(used > 0, msg);
    }
#endif
    printf("   %s\n", fails == f0 ? "ok" : "FAILED");
  }
  printf("TOTAL FAILS=%d  checks=%d\n", fails, checks);
  return fails ? 1 : 0;
}
