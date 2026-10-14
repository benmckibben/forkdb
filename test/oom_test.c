/* OOM injection tests: verify the store layer handles allocation failures
 * gracefully — no crashes, no leaks, no corruption, clean error codes.
 * Uses real files (io_file.c) to avoid simdisk snapshot fragility.
 *
 * Build: gcc -Wall -Wextra -std=gnu11 -O1 -g -DOOM_INJECT -DFK_MAX_BRANCHES=64
 *   -Istore -Itest test/oom_test.c store/fkstore.c store/io_file.c
 *   store/io_prealloc.c -o build/oom_test -lpthread -lm
 */
#define _GNU_SOURCE
#ifndef OOM_INJECT
#define OOM_INJECT
#endif
#include "oom_harness.h"
#include "../store/fkstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* One copy for every translation unit that includes oom_harness.h. */
volatile int oom_counter = -1;
volatile int oom_total = 0;

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *e(int rc) {
  switch (rc) {
    case FK_OK: return "FK_OK"; case FK_ENOMEM: return "FK_ENOMEM";
    case FK_EINVAL: return "FK_EINVAL"; case FK_EBUSY: return "FK_EBUSY";
    case FK_ELIMIT: return "FK_ELIMIT"; case FK_EIO: return "FK_EIO";
    case FK_ECORRUPT: return "FK_ECORRUPT"; case FK_ENOENT: return "FK_ENOENT";
    case FK_EFAILED: return "FK_EFAILED"; case FK_EVERSION: return "FK_EVERSION";
    default: return "?";
  }
}

static char path[256];
static const char *P(const char *n) { snprintf(path, sizeof path, "/tmp/oom_%s.fkdb", n); return path; }

/* Write a minimal store: branch 0, a few pages. Uses real file I/O. */
static int make_store(const char *file) {
  unlink(file);
  fk_io io;
  if (fk_file_open(file, &io) != 0) return FK_EIO;
  oom_fail_after(99999);
  fk_store *s = NULL;
  int rc = fk_open(&io, &s);
  oom_reset();
  if (rc != FK_OK) { fk_file_close(&io); return rc; }
  char page[FK_PAGE];
  memset(page, 'A', sizeof page);
  if ((rc = fk_txn_begin(s, 0)) != FK_OK) { fk_close(s); fk_file_close(&io); return rc; }
  if ((rc = fk_txn_write(s, 1, page)) != FK_OK) { goto abort; }
  memset(page, 'B', sizeof page);
  if ((rc = fk_txn_write(s, 2, page)) != FK_OK) { goto abort; }
  rc = fk_txn_commit(s, 3);
  if (rc != FK_OK) goto abort;
  fk_close(s);
  fk_file_close(&io);
  return FK_OK;
abort:
  fk_txn_abort(s);
  fk_close(s);
  fk_file_close(&io);
  return rc;
}

/* Build a store with several branches and overlapping page changes. */
static int build_layer_tree(const char *file) {
  unlink(file);
  fk_io io;
  if (fk_file_open(file, &io) != 0) return FK_EIO;
  oom_fail_after(99999);
  fk_store *s = NULL;
  int rc = fk_open(&io, &s);
  oom_reset();
  if (rc != FK_OK) { fk_file_close(&io); return rc; }
  char page[FK_PAGE];
  memset(page, 'X', sizeof page);
  if ((rc = fk_txn_begin(s, 0)) != FK_OK) goto fail;
  for (uint32_t p = 1; p <= 5; p++) {
    page[0] = (char)('0' + p);
    if ((rc = fk_txn_write(s, p, page)) != FK_OK) goto fail;
  }
  if ((rc = fk_txn_commit(s, 6)) != FK_OK) goto fail;
  /* fork, modify, commit */
  uint32_t b1;
  if ((rc = fk_fork(s, 0, &b1)) != FK_OK) goto fail;
  if ((rc = fk_txn_begin(s, b1)) != FK_OK) goto fail;
  page[0] = 'Y';
  if ((rc = fk_txn_write(s, 2, page)) != FK_OK) goto fail;
  if ((rc = fk_txn_commit(s, 6)) != FK_OK) goto fail;
  /* another fork with more changes */
  uint32_t b2;
  if ((rc = fk_fork(s, 0, &b2)) != FK_OK) goto fail;
  if ((rc = fk_txn_begin(s, b2)) != FK_OK) goto fail;
  page[0] = 'Z';
  for (uint32_t p = 3; p <= 7; p++) {
    page[0] = (char)('Z' + p - 3);
    if ((rc = fk_txn_write(s, p, page)) != FK_OK) goto fail;
  }
  if ((rc = fk_txn_commit(s, 8)) != FK_OK) goto fail;
  fk_close(s);
  fk_file_close(&io);
  return FK_OK;
fail:
  fk_txn_abort(s);
  fk_close(s);
  fk_file_close(&io);
  return rc;
}

/* fk_open must survive OOM during scan/layer init. */
static void test_open_oom(void) {
  printf("open OOM\n");
  int rc = make_store(P("open"));
  CHECK(rc == FK_OK, "setup: %s", e(rc));
  if (rc != FK_OK) return;
  int saw_oom = 0, saw_ok = 0;
  for (int fail_after = 0; fail_after < 200; fail_after++) {
    fk_io io;
    if (fk_file_open(P("open"), &io) != 0) continue;
    oom_fail_after(fail_after);
    fk_store *s = NULL;
    rc = fk_open(&io, &s);
    oom_reset();
    CHECK(rc == FK_OK || rc == FK_ENOMEM, "open(fail_at=%d) = %s", fail_after, e(rc));
    if (rc == FK_ENOMEM) saw_oom = 1;
    if (rc == FK_OK) saw_ok = 1;
    if (rc == FK_OK) {
      char buf[FK_PAGE];
      int rrc = fk_read(s, 0, 1, buf);
      CHECK(rrc == FK_OK || rrc == FK_EFAILED, "read: %s", e(rrc));
      fk_close(s);
    }
    fk_file_close(&io);
  }
  CHECK(saw_oom, "open OOM was never injected");
  CHECK(saw_ok, "open never succeeded with injection armed");
  unlink(P("open"));
}

/* fk_fork survives OOM in layer_new during apply_fork. */
static void test_fork_oom(void) {
  printf("fork OOM\n");
  int rc = make_store(P("fork"));
  CHECK(rc == FK_OK, "setup: %s", e(rc));
  if (rc != FK_OK) return;
  int saw_oom = 0, saw_ok = 0;
  for (int fail_after = 0; fail_after < 100; fail_after++) {
    fk_io io;
    if (fk_file_open(P("fork"), &io) != 0) continue;
    oom_fail_after(99999);
    fk_store *s = NULL;
    rc = fk_open(&io, &s);
    oom_reset();
    if (rc != FK_OK) { fk_file_close(&io); continue; }
    uint32_t layers = fk_layer_count(s), branches = fk_branch_count(s);
    struct stat st_before;
    if (stat(P("fork"), &st_before) != 0) { fk_close(s); fk_file_close(&io); continue; }
    oom_fail_after(fail_after);
    uint32_t kid;
    rc = fk_fork(s, 0, &kid);
    oom_reset();
    if (rc == FK_ENOMEM) saw_oom = 1;
    if (rc == FK_OK) saw_ok = 1;
    if (rc == FK_ENOMEM) {
      struct stat st_after;
      CHECK(!fk_is_failed(s), "fork OOM at %d fail-stopped", fail_after);
      CHECK(fk_layer_count(s) == layers && fk_branch_count(s) == branches,
            "fork OOM at %d changed the branch table", fail_after);
      CHECK(stat(P("fork"), &st_after) == 0 && st_after.st_size == st_before.st_size,
            "fork OOM at %d left a durable record", fail_after);
    }
    fk_close(s);
    fk_file_close(&io);
  }
  CHECK(saw_oom, "fork OOM was never injected");
  CHECK(saw_ok, "fork never succeeded with injection armed");
  unlink(P("fork"));
}

/* fk_txn_commit survives OOM in put_rec (record buffer alloc). */
static void test_commit_oom(void) {
  printf("commit OOM\n");
  int rc = make_store(P("commit"));
  CHECK(rc == FK_OK, "setup: %s", e(rc));
  if (rc != FK_OK) return;
  char page[FK_PAGE];
  memset(page, 'C', sizeof page);
  int saw_oom = 0, saw_ok = 0;
  for (int fail_after = 0; fail_after < 20; fail_after++) {
    fk_io io;
    if (fk_file_open(P("commit"), &io) != 0) continue;
    oom_fail_after(99999);
    fk_store *s = NULL;
    int rrc = fk_open(&io, &s);
    oom_reset();
    if (rrc != FK_OK) { fk_file_close(&io); continue; }
    rrc = fk_txn_begin(s, 0);
    if (rrc != FK_OK) { fk_close(s); fk_file_close(&io); continue; }
    rrc = fk_txn_write(s, 3, page);
    if (rrc != FK_OK) { fk_txn_abort(s); fk_close(s); fk_file_close(&io); continue; }
    oom_fail_after(fail_after);
    rrc = fk_txn_commit(s, 4);
    oom_reset();
    if (rrc == FK_ENOMEM) saw_oom = 1;
    if (rrc == FK_OK) saw_ok = 1;
    if (rrc == FK_ENOMEM)
      CHECK(!fk_is_failed(s), "commit OOM fail_after=%d fail-stopped", fail_after);
    /* Commit leaves the transaction open on any error, including FK_ENOMEM. */
    if (rrc != FK_OK) fk_txn_abort(s);
    fk_close(s);
    fk_file_close(&io);
  }
  CHECK(saw_oom, "commit OOM was never injected");
  CHECK(saw_ok, "commit never succeeded with injection armed");
  unlink(P("commit"));
}

/* Compaction / plan_build survives OOM. Tested via fk_stats (calls plan_build). */
static void test_compact_oom(void) {
  printf("compact OOM\n");
  int rc = build_layer_tree(P("compact"));
  CHECK(rc == FK_OK, "setup: %s", e(rc));
  if (rc != FK_OK) return;
  int saw_oom = 0, saw_ok = 0;
  for (int fail_after = 0; fail_after < 200; fail_after++) {
    fk_io io;
    if (fk_file_open(P("compact"), &io) != 0) continue;
    oom_fail_after(99999);
    fk_store *s = NULL;
    rc = fk_open(&io, &s);
    oom_reset();
    if (rc != FK_OK) { fk_file_close(&io); continue; }
    oom_fail_after(fail_after);
    uint64_t file_bytes, live_bytes;
    rc = fk_stats(s, &file_bytes, &live_bytes);
    oom_reset();
    CHECK(rc == FK_OK || rc == FK_ENOMEM, "fk_stats(fail_at=%d) = %s", fail_after, e(rc));
    if (rc == FK_ENOMEM) saw_oom = 1;
    if (rc == FK_OK) saw_ok = 1;
    if (rc == FK_ENOMEM)
      CHECK(!fk_is_failed(s), "stats OOM at %d fail-stopped", fail_after);
    fk_close(s);
    fk_file_close(&io);
  }
  CHECK(saw_oom, "compact OOM was never injected");
  CHECK(saw_ok, "compact never succeeded with injection armed");
  unlink(P("compact"));
}

int main(void) {
  test_open_oom();
  test_fork_oom();
  test_commit_oom();
  test_compact_oom();
  printf("%d checks, TOTAL FAILS=%d\n", checks, fails);
  return fails ? 1 : 0;
}
