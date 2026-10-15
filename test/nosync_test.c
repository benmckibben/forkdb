/* nosync_test: prove that a durable open issues syncs and a nosync open issues none, by counting real system calls through
 * GNU linker --wrap. Also proves data integrity after nosync sessions and process-death survival. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../store/fkstore.h"

/* -- wrap counters -------------------------------------------------------- */
static long sync_calls;

/* The __real_ functions are provided by the linker. */
int __real_fsync(int fd);
int __real_fdatasync(int fd);

int __wrap_fsync(int fd) {
  sync_calls++;
  return __real_fsync(fd);
}

int __wrap_fdatasync(int fd) {
  sync_calls++;
  return __real_fdatasync(fd);
}

static long read_syncs(void) {
  /* Compiler barrier: make sure the counter is read from memory. */
  long v;
  __atomic_load(&sync_calls, &v, __ATOMIC_RELAXED);
  return v;
}

/* -- helpers -------------------------------------------------------------- */
static int fails, checks;

#define CHECK(c, msg) do {                     \
  int ok_ = !!(c);                             \
  checks++;                                    \
  printf("  %s %s\n", ok_ ? "PASS" : "FAIL", msg); \
  if (!ok_) fails++;                           \
} while (0)

static void page(uint8_t *p, int v) {
  for (int i = 0; i < (int)FK_PAGE; i++) p[i] = (uint8_t)(v * 31 + i);
}

static int has(fk_store *s, uint32_t b, uint32_t pg, int v) {
  uint8_t a[FK_PAGE], e[FK_PAGE];
  page(e, v);
  return fk_read(s, b, pg, a) == 0 && !memcmp(a, e, FK_PAGE);
}

static int commit1(fk_store *s, uint32_t b, uint32_t pg, int v, uint32_t size) {
  uint8_t p[FK_PAGE];
  page(p, v);
  return fk_txn_begin(s, b) || fk_txn_write(s, pg, p) || fk_txn_commit(s, size);
}

static int open_path_ex(const char *path, fk_io *io, fk_store **s, int flags) {
  return fk_file_open_ex(path, io, flags) == 0 && fk_open(io, s) == 0;
}

int main(void) {
  char dir[] = "/tmp/fknosyncXXXXXX";
  if (!mkdtemp(dir)) return 2;
  char path[300];
  snprintf(path, sizeof path, "%s/store.fk", dir);

  fk_io io;
  fk_store *s = NULL;
  long before = 0, after = 0;

  printf("== Check 1: DURABLE control – a normal open (flags 0) issues syncs\n");
  before = read_syncs();
  CHECK(open_path_ex(path, &io, &s, 0), "open durable (flags=0)");
  CHECK(fk_prealloc_wrap(&io, FK_PREALLOC_DEFAULT) == 0, "preallocation layer");
  {
    int ok = 1, ncommits = 20;
    for (int i = 0; i < ncommits; i++) {
      ok = ok && commit1(s, 0, (uint32_t)(i % 5), 100 + i, 5) == 0;
    }
    CHECK(ok, "20 commits");
    uint32_t f = 0;
    CHECK(fk_fork(s, 0, &f) == 0 && f == 1 && commit1(s, 1, 1, 77, 5) == 0, "fork and write on it");
    CHECK(fk_discard(s, 1) == 0, "discard the fork");
    /* Do a meta-set (another synced operation) before compact */
    CHECK(fk_meta_set(s, 0, "test", 4) == 0, "metadata set");
    CHECK(fk_compact(s) == 0, "compact");
  }
  fk_close(s);
  fk_prealloc_close(&io);
  fk_file_close(&io);
  after = read_syncs();
  printf("    (sync calls went from %ld to %ld, delta = %ld)\n", before, after, after - before);
  CHECK(after - before >= 20, "sync count increased by at least 20");

  printf("== Check 2: NOSYNC – the counter does not move at all through an entire session\n");
  /* 2a: without the preallocation layer */
  snprintf(path, sizeof path, "%s/nosync.fk", dir);
  before = read_syncs();
  CHECK(open_path_ex(path, &io, &s, FK_FILE_NOSYNC), "open nosync");
  {
    int ok = 1;
    for (int i = 0; i < 20; i++) {
      ok = ok && commit1(s, 0, (uint32_t)(i % 5), 100 + i, 5) == 0;
    }
    CHECK(ok, "20 commits nosync");
    uint32_t f = 0;
    CHECK(fk_fork(s, 0, &f) == 0 && f == 1 && commit1(s, 1, 1, 77, 5) == 0, "fork and write");
    CHECK(fk_discard(s, 1) == 0, "discard");
    CHECK(fk_meta_set(s, 0, "test", 4) == 0, "metadata set");
    CHECK(fk_compact(s) == 0, "compact without prealloc");
  }
  fk_close(s);
  fk_file_close(&io);
  after = read_syncs();
  CHECK(after == before, "sync counter did not move during nosync session (without prealloc)");

  /* 2b: with the preallocation layer */
  snprintf(path, sizeof path, "%s/nosync2.fk", dir);
  before = read_syncs();
  CHECK(open_path_ex(path, &io, &s, FK_FILE_NOSYNC), "open nosync with prealloc");
  CHECK(fk_prealloc_wrap(&io, FK_PREALLOC_DEFAULT) == 0, "preallocation layer");
  {
    int ok = 1;
    for (int i = 0; i < 20; i++) {
      ok = ok && commit1(s, 0, (uint32_t)(i % 5), 200 + i, 5) == 0;
    }
    CHECK(ok, "20 commits nosync through prealloc");
    uint32_t f = 0;
    CHECK(fk_fork(s, 0, &f) == 0 && commit1(s, f, 1, 88, 5) == 0, "fork and write");
    CHECK(fk_discard(s, f) == 0 && fk_compact(s) == 0, "discard and compact");
  }
  fk_close(s);
  fk_prealloc_close(&io);
  fk_file_close(&io);
  after = read_syncs();
  CHECK(after == before, "sync counter did not move during nosync session (with prealloc)");

  printf("== Check 3: data is intact after a nosync session\n");
  /* Write in nosync, reopen durably, verify every piece */
  snprintf(path, sizeof path, "%s/roundtrip.fk", dir);
  CHECK(open_path_ex(path, &io, &s, FK_FILE_NOSYNC), "nosync open for writing");
  {
    commit1(s, 0, 0, 1, 3);
    commit1(s, 0, 1, 2, 3);
    commit1(s, 0, 2, 3, 3);
    uint32_t f1 = 0, f2 = 0;
    fk_fork(s, 0, &f1);
    commit1(s, 1, 0, 9, 3);
    commit1(s, 1, 1, 10, 3);
    fk_fork(s, 0, &f2);
    commit1(s, 2, 0, 55, 3);
    fk_discard(s, 2);
    commit1(s, 0, 0, 100, 3);
  }
  fk_close(s);
  fk_file_close(&io);
  /* Reopen durably and check */
  CHECK(open_path_ex(path, &io, &s, 0), "reopen durable");
  CHECK(has(s, 0, 0, 100) && has(s, 0, 1, 2) && has(s, 0, 2, 3), "main reads back: p0=100, p1=2, p2=3");
  CHECK(has(s, 1, 0, 9) && has(s, 1, 1, 10) && has(s, 1, 2, 3), "fork reads back: p0=9, p1=10, shared p2=3");
  CHECK(!fk_branch_alive(s, 2) && fk_branch_count(s) == 3, "branch 2 is discarded, branch count is 3");
  /* Compact while open durable, verify again */
  CHECK(fk_compact(s) == 0, "compact durable");
  CHECK(has(s, 0, 0, 100) && has(s, 0, 1, 2) && has(s, 0, 2, 3), "after compact: main intact");
  CHECK(has(s, 1, 0, 9) && has(s, 1, 1, 10) && has(s, 1, 2, 3), "after compact: fork intact");
  /* Write more, reopen nosync, read all */
  commit1(s, 0, 0, 101, 3);
  commit1(s, 1, 1, 11, 3);
  fk_close(s);
  fk_file_close(&io);
  CHECK(open_path_ex(path, &io, &s, FK_FILE_NOSYNC), "reopen nosync");
  CHECK(has(s, 0, 0, 101) && has(s, 1, 1, 11) && has(s, 0, 2, 3) && has(s, 1, 0, 9), "nosync reopen reads all: p0(0)=101, p1(1)=11, p2=3, p0(1)=9");
  fk_close(s);
  fk_file_close(&io);

  printf("== Check 4: process death – data survives a kill(-9) with nosync\n");
  /* 4a: child commits 50, kills itself without closing, parent verifies */
  snprintf(path, sizeof path, "%s/killed.fk", dir);
  {
    pid_t pid = fork();
    if (pid == 0) {
      fk_io ci;
      fk_store *cs;
      if (fk_file_open_ex(path, &ci, FK_FILE_NOSYNC) != 0) _exit(3);
      if (fk_open(&ci, &cs) != 0) _exit(4);
      int ok = 1;
      for (int i = 0; i < 50; i++) {
        ok = ok && commit1(cs, 0, (uint32_t)(i % 5), 500 + i, 5) == 0;
      }
      if (!ok) _exit(5);
      /* Die without closing anything (hold the lock so the parent knows nothing else wrote). */
      kill(getpid(), SIGKILL);
      _exit(99); /* not reached */
    }
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid, "parent waits for child");
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "child was killed by SIGKILL (not exited)");
    /* Open durably: the lock should have been released. */
    CHECK(open_path_ex(path, &io, &s, 0), "reopen after kill");
    int ok = 1;
    /* Each page gets written multiple times; verify the LAST value. Pages 0-4 are written in cycles,
     * final writes: i=45..49 → pages 0..4 with values 545..549. But also check pages 0-4 each have correct final value. */
    for (int pg = 0; pg < 5 && ok; pg++) {
      /* Find the last write to page pg: it's at i where i%5 == pg, largest i < 50 */
      int last_v = 500 + 45 + pg; /* largest i is 49, so for pg 4: 500+49=549; for pg 0: 500+45=545 */
      ok = has(s, 0, (uint32_t)pg, last_v);
    }
    CHECK(ok, "all 50 acknowledged commits present after the nosync kill");
    /* Also check the store's internal consistency: after a kill it should survive a compaction. */
    CHECK(fk_compact(s) == 0, "compact after kill recovery works");
    ok = 1;
    /* Same check after compaction: verify 5 pages with final values */
    for (int pg = 0; pg < 5 && ok; pg++) {
      int last_v = 500 + 45 + pg;
      ok = has(s, 0, (uint32_t)pg, last_v);
    }
    CHECK(ok, "all 50 commits still present after compaction");
    fk_close(s);
    fk_file_close(&io);
  }

  /* 4b: child killed mid-transaction: the uncommitted data is absent */
  snprintf(path, sizeof path, "%s/midtxn.fk", dir);
  {
    pid_t pid = fork();
    if (pid == 0) {
      fk_io ci;
      fk_store *cs;
      if (fk_file_open_ex(path, &ci, FK_FILE_NOSYNC) != 0) _exit(3);
      if (fk_open(&ci, &cs) != 0) _exit(4);
      /* Commit page 0 = 10, page 1 = 20 */
      commit1(cs, 0, 0, 10, 2);
      commit1(cs, 0, 1, 20, 2);
      /* Begin a transaction for page 0 = 99, never commit it */
      fk_txn_begin(cs, 0);
      uint8_t p[FK_PAGE];
      page(p, 99);
      fk_txn_write(cs, 0, p);
      /* Kill without committing */
      kill(getpid(), SIGKILL);
      _exit(99);
    }
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid, "parent waits for mid-txn child");
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "child killed by SIGKILL");
    CHECK(open_path_ex(path, &io, &s, 0), "reopen after mid-txn kill");
    CHECK(has(s, 0, 0, 10), "committed page 0 is present");
    CHECK(has(s, 0, 1, 20), "committed page 1 is present");
    /* The uncommitted transaction must be absent: page 0 must still be 10, not 99 */
    CHECK(!has(s, 0, 0, 99), "uncommitted page 0=99 is NOT present (it was never committed)");
    fk_close(s);
    fk_file_close(&io);
  }

  printf("== Check 5: the lock still applies with nosync\n");
  snprintf(path, sizeof path, "%s/lockns.fk", dir);
  CHECK(open_path_ex(path, &io, &s, FK_FILE_NOSYNC), "open nosync holding the lock");
  CHECK(commit1(s, 0, 0, 7, 1) == 0, "commit one page");
  {
    fk_io io2;
    CHECK(fk_file_open_ex(path, &io2, FK_FILE_NOSYNC) == FK_ELOCKED, "second nosync open is refused with FK_ELOCKED");
    CHECK(fk_file_open_ex(path, &io2, 0) == FK_ELOCKED, "second durable open is refused with FK_ELOCKED");
    pid_t pid = fork();
    if (pid == 0) {
      fk_io ci;
      _exit(fk_file_open_ex(path, &ci, FK_FILE_NOSYNC) == FK_ELOCKED ? 0 : 3);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "another process is refused with FK_ELOCKED while nosync holds it");
  }
  CHECK(has(s, 0, 0, 7), "data unchanged");
  fk_close(s);
  fk_file_close(&io);
  /* After close, reopening works */
  CHECK(open_path_ex(path, &io, &s, FK_FILE_NOSYNC) && has(s, 0, 0, 7), "after close, reopen nosync succeeds");
  fk_close(s);
  fk_file_close(&io);
  CHECK(open_path_ex(path, &io, &s, 0) && has(s, 0, 0, 7), "after close, reopen durable succeeds");
  fk_close(s);
  fk_file_close(&io);

  printf("\nTOTAL FAILS=%d  checks=%d\n", fails, checks);
  char cmd[400];
  snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
  (void)!system(cmd);
  return fails ? 1 : 0;
}
