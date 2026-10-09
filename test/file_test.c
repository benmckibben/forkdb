/* file_test: the real-file backend. No power cut here (see crash_test for that): a normal close/reopen, a process that
 * dies without closing, a torn tail appended to the file, and a damaged page found at read time. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../store/fkstore.h"

static int fails;
#define CHECK(c, msg) do { int ok_ = !!(c); printf("  %s %s\n", ok_ ? "PASS" : "FAIL", msg); if (!ok_) fails++; } while (0)

static void page(uint8_t *p, int v) { for (int i = 0; i < (int)FK_PAGE; i++) p[i] = (uint8_t)(v * 31 + i); }
static int has(fk_store *s, uint32_t b, uint32_t pg, int v) { uint8_t a[FK_PAGE], e[FK_PAGE]; page(e, v); return fk_read(s, b, pg, a) == 0 && !memcmp(a, e, FK_PAGE); }
static int commit1(fk_store *s, uint32_t b, uint32_t pg, int v, uint32_t size) { uint8_t p[FK_PAGE]; page(p, v); return fk_txn_begin(s, b) || fk_txn_write(s, pg, p) || fk_txn_commit(s, size); }
static int open_path(const char *path, fk_io *io, fk_store **s) { return fk_file_open(path, io) == 0 && fk_open(io, s) == 0; }

int main(void) {
  char dir[] = "/tmp/fkfileXXXXXX";
  if (!mkdtemp(dir)) return 2;
  char path[300];
  snprintf(path, sizeof path, "%s/store.fk", dir);
  fk_io io; fk_store *s = NULL;

  printf("== create, commit, fork, close, reopen\n");
  CHECK(open_path(path, &io, &s), "create");
  CHECK(commit1(s, 0, 0, 1, 2) == 0 && commit1(s, 0, 1, 2, 2) == 0, "two commits on main");
  uint32_t f = 0;
  CHECK(fk_fork(s, 0, &f) == 0 && f == 1, "fork");
  CHECK(commit1(s, 1, 0, 9, 2) == 0, "write on the fork");
  CHECK(has(s, 0, 0, 1) && has(s, 1, 0, 9) && has(s, 1, 1, 2), "fork isolated from main, shares unchanged pages");
  fk_close(s); fk_file_close(&io);
  CHECK(open_path(path, &io, &s) && has(s, 0, 0, 1) && has(s, 0, 1, 2) && has(s, 1, 0, 9) && has(s, 1, 1, 2), "reopen: everything is back");
  struct stat st; stat(path, &st);
  long committed = (long)st.st_size;
  fk_close(s); fk_file_close(&io);

  printf("== a process that dies without closing\n");
  pid_t pid = fork();
  if (pid == 0) {
    fk_io io2; fk_store *s2;
    if (!open_path(path, &io2, &s2)) _exit(3);
    for (int i = 0; i < 5; i++) if (commit1(s2, 0, 0, 100 + i, 2)) _exit(4);
    _exit(0); /* no fk_close */
  }
  int status = 0; waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child committed 5 times and exited without closing");
  CHECK(open_path(path, &io, &s) && has(s, 0, 0, 104) && has(s, 1, 0, 9), "reopen sees all 5 commits");
  fk_close(s); fk_file_close(&io);

  printf("== a torn tail (garbage and a half-written record) is cut off\n");
  stat(path, &st);
  long before = (long)st.st_size;
  int fd = open(path, O_WRONLY | O_APPEND);
  uint8_t junk[300]; for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 7);
  memcpy(junk, "FKR1", 4); /* looks like a record start, is not valid */
  CHECK(write(fd, junk, sizeof junk) == (ssize_t)sizeof junk, "append 300 bytes of junk");
  close(fd);
  CHECK(open_path(path, &io, &s) && has(s, 0, 0, 104), "reopen: committed data intact");
  stat(path, &st);
  CHECK((long)st.st_size == before, "recovery truncated the file back to the last committed byte");
  CHECK(commit1(s, 0, 1, 77, 2) == 0, "and the store accepts commits afterwards");
  fk_close(s); fk_file_close(&io);
  CHECK(open_path(path, &io, &s) && has(s, 0, 1, 77), "that commit survives a reopen");
  fk_close(s); fk_file_close(&io);
  (void)committed;

  printf("== a valid record from another history must not be accepted as a continuation\n");
  {
    char pa[300], pb[300];
    snprintf(pa, sizeof pa, "%s/histA.fk", dir);
    snprintf(pb, sizeof pb, "%s/histB.fk", dir);
    fk_io ia, ib; fk_store *sa = NULL, *sb = NULL;
    CHECK(open_path(pa, &ia, &sa) && commit1(sa, 0, 0, 5, 1) == 0, "history A: one commit");
    fk_close(sa); fk_file_close(&ia);
    CHECK(open_path(pb, &ib, &sb) && fk_fork(sb, 0, &f) == 0, "history B: one fork (a single valid FORK record, sequence 1)");
    fk_close(sb); fk_file_close(&ib);
    uint8_t rec[40];
    int fb = open(pb, O_RDONLY), fa = open(pa, O_RDWR | O_APPEND);
    CHECK(fb >= 0 && fa >= 0 && pread(fb, rec, sizeof rec, 4096) == (ssize_t)sizeof rec, "read that record");
    CHECK(write(fa, rec, sizeof rec) == (ssize_t)sizeof rec, "append it to the end of history A");
    close(fb); close(fa);
    CHECK(open_path(pa, &ia, &sa) && fk_branch_count(sa) == 1 && has(sa, 0, 0, 5), "recovery rejects it: it does not continue A's record chain");
    if (sa) fk_close(sa);
    fk_file_close(&ia);
  }

  printf("== damage inside committed data is reported, not silently cut off\n");
  fd = open(path, O_RDWR);
  /* Layout: header 4096, then commit 1 = PAGE (4136 bytes) + COMMIT (52 bytes), then the PAGE of commit 2 (main page 1,
   * still read by the fork), so its data starts at 4096 + 4136 + 52 + 32 + 8. */
  uint64_t dmg = 4096 + 4136 + 52 + 32 + 8 + 100;
  uint8_t b = 0;
  if (pread(fd, &b, 1, (off_t)dmg) != 1) return 2;
  b ^= 0xff;
  if (pwrite(fd, &b, 1, (off_t)dmg) != 1) return 2;
  close(fd);
  stat(path, &st);
  long dmg_size = (long)st.st_size;
  s = NULL;
  CHECK(fk_file_open(path, &io) == 0 && fk_open(&io, &s) == FK_ECORRUPT, "open refuses: committed data (covered by the durable-end marker) is damaged, which is not a torn tail");
  stat(path, &st);
  CHECK((long)st.st_size == dmg_size, "and it did not truncate the file: nothing is cut off, so the data can still be salvaged");
  fk_file_close(&io);

  printf("== an empty or truncated file is a new store; a foreign file is refused\n");
  snprintf(path, sizeof path, "%s/empty.fk", dir);
  fd = open(path, O_CREAT | O_RDWR, 0644); close(fd);
  CHECK(open_path(path, &io, &s) && fk_branch_count(s) == 1, "zero-length file opens as a new store");
  fk_close(s); fk_file_close(&io);
  snprintf(path, sizeof path, "%s/foreign.fk", dir);
  fd = open(path, O_CREAT | O_RDWR, 0644);
  uint8_t big[8192]; memset(big, 0x5a, sizeof big); (void)!write(fd, big, sizeof big); close(fd);
  CHECK(fk_file_open(path, &io) == 0 && fk_open(&io, &s) == FK_ECORRUPT, "a file with someone else's data is FK_ECORRUPT, not overwritten");
  fk_file_close(&io);

  printf("\nTOTAL FAILS=%d\n", fails);
  char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)!system(cmd);
  return fails ? 1 : 0;
}
