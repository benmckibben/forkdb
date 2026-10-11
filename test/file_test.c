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
static long fsz(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }
static int exists(const char *p) { return access(p, F_OK) == 0; }

/* io wrappers that misbehave at one point of a compaction */
static int (*real_rename)(void *, fk_io *);
static int (*real_create)(void *, fk_io *);
static int rename_then_die(void *c, fk_io *tmp) { if (real_rename(c, tmp) == 0) _exit(0); return -1; } /* renamed, directory not synced */
static int die_before_rename(void *c, fk_io *tmp) { (void)c; (void)tmp; _exit(0); }                       /* new file complete, not swapped in */
static int create_fails(void *c, fk_io *tmp) { (void)c; (void)tmp; return -1; }
static int write_fails(void *c, uint64_t off, const void *b, size_t n) { (void)c; (void)off; (void)b; (void)n; return -1; } /* like ENOSPC */
static int create_then_failing_writes(void *c, fk_io *tmp) { if (real_create(c, tmp)) return -1; tmp->pwrite = write_fails; return 0; }
static void patch_header_version(const char *path, uint32_t v) {
  uint8_t h[20];
  int fd = open(path, O_RDWR);
  if (fd < 0 || pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h) exit(2);
  for (int i = 0; i < 4; i++) h[8 + i] = (uint8_t)(v >> (8 * i));
  uint32_t c = fk_crc32c(0, h, 16);
  for (int i = 0; i < 4; i++) h[16 + i] = (uint8_t)(c >> (8 * i));
  if (pwrite(fd, h, sizeof h, 0) != (ssize_t)sizeof h) exit(2);
  close(fd);
}

/* A store with a lot of garbage: page 0 of main rewritten 200 times, a fork with its own page 1, then main's page 1 changed and a
 * second fork taken and discarded. That leaves a frozen layer (holding main's page 1 = 9) with exactly one child, which is the
 * case compaction folds: if folding lost that layer's pages, main's page 1 would read 7 again. */
static int build_garbage(const char *path, fk_io *io, fk_store **s) {
  uint32_t f1 = 0, f2 = 0;
  if (!open_path(path, io, s)) return 0;
  if (commit1(*s, 0, 1, 7, 2)) return 0;
  for (int i = 0; i < 200; i++) if (commit1(*s, 0, 0, 1000 + i, 2)) return 0;
  if (fk_fork(*s, 0, &f1) || commit1(*s, 1, 1, 8, 2)) return 0;
  if (commit1(*s, 0, 1, 9, 2) || fk_fork(*s, 0, &f2) || commit1(*s, 2, 0, 55, 2) || fk_discard(*s, 2)) return 0;
  return commit1(*s, 0, 0, 2000, 2) == 0;
}
static int garbage_state_ok(fk_store *s) {
  return has(s, 0, 0, 2000) && has(s, 0, 1, 9) && has(s, 1, 0, 1199) && has(s, 1, 1, 8) && !fk_branch_alive(s, 2) && fk_branch_count(s) == 3;
}

/* CRC-32C reference: one bit at a time, no tables, nothing shared with the implementation under test. */
static uint32_t crc_ref(uint32_t crc, const uint8_t *b, size_t n) {
  uint32_t c = ~crc;
  while (n--) {
    c ^= *b++;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
  }
  return ~c;
}
static uint64_t xs_state = 88172645463325252ull;
static uint64_t xs(void) { xs_state ^= xs_state << 13; xs_state ^= xs_state >> 7; xs_state ^= xs_state << 17; return xs_state; }

/* Returns the number of failures for the implementation currently selected. */
static int crc_checks(const char *name) {
  int bad = 0;
  uint8_t v[32];
  struct { const char *what; const uint8_t *p; size_t n; uint32_t want; } vec[5];
  uint8_t zeros[32] = {0}, ones[32], asc[32], desc[32];
  memset(ones, 0xff, sizeof ones);
  for (int i = 0; i < 32; i++) { asc[i] = (uint8_t)i; desc[i] = (uint8_t)(31 - i); }
  vec[0] = (typeof(vec[0])){"32 zero bytes", zeros, 32, 0x8a9136aau};
  vec[1] = (typeof(vec[0])){"32 bytes of 0xff", ones, 32, 0x62a8ab43u};
  vec[2] = (typeof(vec[0])){"bytes 0..31", asc, 32, 0x46dd794eu};
  vec[3] = (typeof(vec[0])){"bytes 31..0", desc, 32, 0x113fdb5cu};
  vec[4] = (typeof(vec[0])){"\"123456789\"", (const uint8_t *)"123456789", 9, 0xe3069283u};
  (void)v;
  for (int i = 0; i < 5; i++) if (fk_crc32c(0, vec[i].p, vec[i].n) != vec[i].want) { printf("    %s: %s is %08x, expected %08x\n", name, vec[i].what, fk_crc32c(0, vec[i].p, vec[i].n), vec[i].want); bad++; }
  /* random lengths (0..9000, so every n % 8) at every alignment, random starting value; chaining must equal one call */
  static uint8_t buf[9100];
  long mism = 0, chain = 0;
  for (int i = 0; i < 20000; i++) {
    size_t n = (size_t)(xs() % 9000), off = (size_t)(xs() % 8);
    for (size_t j = 0; j < n; j++) buf[off + j] = (uint8_t)xs();
    uint32_t seed = (uint32_t)xs();
    if (fk_crc32c(seed, buf + off, n) != crc_ref(seed, buf + off, n)) mism++;
    size_t cut = n ? (size_t)(xs() % (n + 1)) : 0;
    if (fk_crc32c(fk_crc32c(seed, buf + off, cut), buf + off + cut, n - cut) != crc_ref(seed, buf + off, n)) chain++;
  }
  if (mism || chain) { printf("    %s: %ld of 20000 random buffers differ from the reference, %ld chained calls differ\n", name, mism, chain); bad++; }
  return bad;
}
/* the same operations, so two runs must write the same bytes */
static int small_history(const char *path) {
  fk_io io; fk_store *s = NULL; uint32_t f = 0;
  if (!open_path(path, &io, &s)) return 0;
  int ok = commit1(s, 0, 0, 1, 3) == 0 && commit1(s, 0, 2, 2, 3) == 0 && fk_fork(s, 0, &f) == 0 && commit1(s, 1, 1, 9, 3) == 0 && commit1(s, 0, 0, 5, 3) == 0;
  fk_close(s); fk_file_close(&io);
  return ok;
}
static int same_file(const char *a, const char *b) {
  FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
  int same = fa && fb;
  while (same) {
    unsigned char x[4096], y[4096];
    size_t nx = fread(x, 1, sizeof x, fa), ny = fread(y, 1, sizeof y, fb);
    if (nx != ny || memcmp(x, y, nx)) same = 0;
    if (nx == 0) break;
  }
  if (fa) fclose(fa);
  if (fb) fclose(fb);
  return same;
}

int main(void) {
  char dir[] = "/tmp/fkfileXXXXXX";
  if (!mkdtemp(dir)) return 2;
  char path[300];
  snprintf(path, sizeof path, "%s/store.fk", dir);
  fk_io io; fk_store *s = NULL;

  printf("== CRC-32C: portable and hardware implementations agree with a bit-by-bit reference and with each other\n");
  {
    xs_state = 88172645463325252ull;
    CHECK(fk_crc32c_select(0) == 0 && crc_checks("portable") == 0, "portable implementation: standard test vectors, 20,000 random buffers at every alignment, chained calls");
    xs_state = 88172645463325252ull;
    int hw = fk_crc32c_select(1);
    if (hw) CHECK(crc_checks("hardware") == 0, "hardware implementation: the same checks");
    else printf("  SKIP hardware CRC32C is not available on this CPU or build: only the portable implementation was tested\n");
    char pa[300], pb[300], pc[300];
    snprintf(pa, sizeof pa, "%s/crc_sw.fk", dir); snprintf(pb, sizeof pb, "%s/crc_hw.fk", dir); snprintf(pc, sizeof pc, "%s/crc_sw2.fk", dir);
    fk_crc32c_select(0);
    CHECK(small_history(pa), "write a store with the portable implementation");
    fk_crc32c_select(1);
    CHECK(small_history(pb), "write the same history with the hardware implementation (if any)");
    CHECK(same_file(pa, pb), "the two files are byte for byte identical: no format difference");
    fk_io i2; fk_store *s2 = NULL;
    CHECK(open_path(pa, &i2, &s2) && has(s2, 0, 0, 5) && has(s2, 1, 1, 9) && has(s2, 0, 2, 2), "the portable file is read correctly with the hardware implementation");
    if (s2) fk_close(s2);
    fk_file_close(&i2);
    fk_crc32c_select(0);
    s2 = NULL;
    CHECK(open_path(pb, &i2, &s2) && has(s2, 0, 0, 5) && has(s2, 1, 1, 9) && has(s2, 0, 2, 2), "and the other way round");
    if (s2) fk_close(s2);
    fk_file_close(&i2);
    fk_crc32c_select(1);
    (void)pc;
  }

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

  printf("== compaction: the log shrinks to the live data and every branch still reads the same\n");
  {
    char cpth[300], tpth[420];
    snprintf(cpth, sizeof cpth, "%s/comp.fk", dir);
    snprintf(tpth, sizeof tpth, "%s.compact", cpth);
    CHECK(build_garbage(cpth, &io, &s) && garbage_state_ok(s), "a store with 200 overwritten pages, a fork, and a discarded fork");
    long before2 = fsz(cpth);
    uint64_t fb = 0, lb = 0;
    CHECK(fk_stats(s, &fb, &lb) == 0 && (long)fb == before2 && lb * 10 < fb, "fk_stats: the file is mostly garbage");
    int did = -1;
    CHECK(fk_compact_if_needed(s, 100u << 20, &did) == 0 && did == 0 && fsz(cpth) == before2, "not compacted when the file is below the size threshold");
    CHECK(fk_compact_if_needed(s, 4096, &did) == 0 && did == 1, "compacted when it is big enough and mostly garbage");
    long after = fsz(cpth);
    CHECK(after > 0 && after * 10 < before2, "the file shrank to a small fraction");
    CHECK(garbage_state_ok(s), "same contents for every branch");
    CHECK(fk_stats(s, &fb, &lb) == 0 && fb == lb && (long)fb == after, "the file now holds exactly the live data");
    CHECK(fk_compact_if_needed(s, 4096, &did) == 0 && did == 0, "and is not compacted again");
    CHECK(fk_compact(s) == 0 && fsz(cpth) == after && garbage_state_ok(s), "a forced second compaction changes nothing");
    CHECK(!exists(tpth), "no temporary file left behind");
    CHECK(commit1(s, 1, 0, 3000, 2) == 0 && commit1(s, 0, 1, 4000, 2) == 0, "writes after compaction");
    fk_close(s); fk_file_close(&io);
    CHECK(open_path(cpth, &io, &s) && has(s, 0, 0, 2000) && has(s, 0, 1, 4000) && has(s, 1, 0, 3000) && has(s, 1, 1, 8) && !fk_branch_alive(s, 2), "reopen: compacted data and the later writes are all there");
    uint32_t nf = 0;
    CHECK(fk_fork(s, 1, &nf) == 0 && nf == 3 && has(s, 3, 0, 3000), "branch ids are not reused after compaction; forking works");
    fk_close(s); fk_file_close(&io);

    printf("== compaction: a shrink hides the parent's page, and compaction keeps it hidden\n");
    {
      char tp2[300];
      snprintf(tp2, sizeof tp2, "%s/tomb.fk", dir);
      fk_io it; fk_store *st3 = NULL; uint8_t pg[FK_PAGE], zero[FK_PAGE];
      memset(zero, 0, sizeof zero);
      uint32_t ff = 0;
      CHECK(open_path(tp2, &it, &st3) && commit1(st3, 0, 0, 1, 3) == 0 && commit1(st3, 0, 2, 5, 3) == 0, "main has three pages, page 2 = 5");
      CHECK(fk_fork(st3, 0, &ff) == 0 && commit1(st3, ff, 0, 6, 1) == 0 && commit1(st3, ff, 0, 7, 3) == 0, "a fork shrinks to one page, then grows back to three");
      CHECK(fk_read(st3, ff, 2, pg) == 0 && !memcmp(pg, zero, FK_PAGE) && has(st3, 0, 2, 5), "the fork's page 2 reads as zeros, main's still reads 5");
      CHECK(fk_compact(st3) == 0 && fk_read(st3, ff, 2, pg) == 0 && !memcmp(pg, zero, FK_PAGE) && has(st3, 0, 2, 5) && has(st3, ff, 0, 7), "after compaction: the same");
      fk_close(st3); fk_file_close(&it);
      CHECK(open_path(tp2, &it, &st3) && fk_read(st3, ff, 2, pg) == 0 && !memcmp(pg, zero, FK_PAGE) && has(st3, 0, 2, 5), "and after reopening the compacted file");
      fk_close(st3); fk_file_close(&it);
    }

    printf("== compaction: leftovers and failures\n");
    int fdl = open(tpth, O_CREAT | O_WRONLY, 0644);
    CHECK(fdl >= 0 && write(fdl, "leftover from a crashed compaction", 34) == 34, "plant a leftover temporary file");
    close(fdl);
    CHECK(open_path(cpth, &io, &s) && has(s, 0, 0, 2000), "open succeeds");
    CHECK(!exists(tpth), "open deleted the leftover instead of trusting it");
    fk_close(s); fk_file_close(&io);

    snprintf(cpth, sizeof cpth, "%s/comp2.fk", dir);
    snprintf(tpth, sizeof tpth, "%s.compact", cpth);
    CHECK(build_garbage(cpth, &io, &s), "build a second store");
    long big = fsz(cpth);
    int fd2 = open(cpth, O_RDWR);
    uint8_t byte = 0;
#define DMG (4096 + 200 * 4188 + 32 + 8 + 50)
    /* the PAGE record of main's page 0 = 1199, which the fork still sees: each commit is a 4136-byte PAGE and a 52-byte COMMIT, and it
     * is the 201st commit, so its data starts at 4096 + 200 * 4188 + 32 + 8 */
    CHECK(fd2 >= 0 && pread(fd2, &byte, 1, DMG) == 1, "read a byte of a live page");
    byte ^= 0xff;
    CHECK(pwrite(fd2, &byte, 1, DMG) == 1, "damage it while the store is open");
    int rcd = fk_compact(s);
    CHECK(rcd == FK_ECORRUPT, "compaction refuses to copy a damaged page");
    CHECK(!fk_is_failed(s) && fsz(cpth) == big && !exists(tpth), "the file is untouched, the temporary file is gone, the store is usable");
    CHECK(has(s, 0, 0, 2000) && commit1(s, 0, 0, 2001, 2) == 0, "and still serves the undamaged pages");
    byte ^= 0xff;
    CHECK(pwrite(fd2, &byte, 1, DMG) == 1, "repair the byte");
    close(fd2);
    CHECK(fk_compact(s) == 0 && has(s, 0, 0, 2001) && has(s, 0, 1, 9) && has(s, 1, 0, 1199) && has(s, 1, 1, 8), "after the repair compaction works");
    fk_close(s); fk_file_close(&io);

    snprintf(cpth, sizeof cpth, "%s/comp3.fk", dir);
    snprintf(tpth, sizeof tpth, "%s.compact", cpth);
    CHECK(build_garbage(cpth, &io, &s), "build a third store");
    big = fsz(cpth);
    fk_io bad_io = io;
    real_create = io.sibling_create;
    bad_io.sibling_create = create_fails;
    fk_store *sb = NULL;
    fk_close(s);
    CHECK(fk_open(&bad_io, &sb) == 0 && fk_compact(sb) == FK_EIO && !fk_is_failed(sb) && garbage_state_ok(sb) && fsz(cpth) == big, "cannot create the temporary file: error, store untouched and usable");
    fk_close(sb);
    bad_io.sibling_create = create_then_failing_writes;
    CHECK(fk_open(&bad_io, &sb) == 0 && fk_compact(sb) == FK_EIO && !fk_is_failed(sb) && garbage_state_ok(sb) && fsz(cpth) == big && !exists(tpth),
          "writes to the temporary file fail (like a full disk): error, nothing left behind, store usable");
    CHECK(commit1(sb, 0, 0, 2002, 2) == 0 && has(sb, 0, 0, 2002), "and it keeps accepting commits");
    fk_close(sb); fk_file_close(&io);

    snprintf(cpth, sizeof cpth, "%s/comp4.fk", dir);
    snprintf(tpth, sizeof tpth, "%s.compact", cpth);
    for (int variant = 0; variant < 2; variant++) {
      char pv[400];
      snprintf(pv, sizeof pv, "%s%d", cpth, variant);
      snprintf(tpth, sizeof tpth, "%s.compact", pv);
      CHECK(build_garbage(pv, &io, &s), "build a store, then die in the middle of compacting it");
      big = fsz(pv);
      fk_close(s);
      pid_t pc = fork();
      if (pc == 0) {
        fk_io io3; fk_store *s3;
        if (fk_file_open(pv, &io3)) _exit(3);
        real_rename = io3.sibling_rename;
        io3.sibling_rename = variant == 0 ? die_before_rename : rename_then_die;
        if (fk_open(&io3, &s3)) _exit(4);
        fk_compact(s3);
        _exit(5); /* not reached: the wrapper exits */
      }
      int st2 = 0; waitpid(pc, &st2, 0);
      CHECK(WIFEXITED(st2) && WEXITSTATUS(st2) == 0, variant == 0 ? "the process died after writing the new file, before the rename" : "the process died after the rename, before the directory sync");
      if (variant == 0) CHECK(exists(tpth) && fsz(pv) == big, "the old file is intact and the new file is lying next to it");
      else CHECK(!exists(tpth) && fsz(pv) * 10 < big, "the new file is in place");
      fk_file_close(&io);
      CHECK(open_path(pv, &io, &s) && garbage_state_ok(s), "reopen: every branch reads the same");
      CHECK(!exists(tpth), "no leftover after the reopen");
      fk_close(s); fk_file_close(&io);
    }

    printf("== format versions\n");
    snprintf(cpth, sizeof cpth, "%s/ver1.fk", dir);
    CHECK(open_path(cpth, &io, &s) && commit1(s, 0, 0, 5, 1) == 0, "a store without a snapshot");
    fk_close(s); fk_file_close(&io);
    patch_header_version(cpth, 1);
    CHECK(open_path(cpth, &io, &s) && has(s, 0, 0, 5), "relabelled as version 1, it still opens (nothing in it needs version 2)");
    fk_close(s); fk_file_close(&io);
    snprintf(cpth, sizeof cpth, "%s/ver1c.fk", dir);
    CHECK(build_garbage(cpth, &io, &s) && fk_compact(s) == 0, "a compacted store");
    fk_close(s); fk_file_close(&io);
    patch_header_version(cpth, 1);
    CHECK(fk_file_open(cpth, &io) == 0 && fk_open(&io, &s) == FK_ECORRUPT, "relabelled as version 1, its snapshot is refused (and nothing is cut off)");
    fk_file_close(&io);
  }

  printf("\nTOTAL FAILS=%d\n", fails);
  char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)!system(cmd);
  return fails ? 1 : 0;
}
