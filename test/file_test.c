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

#ifndef FK_TEST_DATA
#define FK_TEST_DATA "test/data" /* where v2-compacted.fk is; scripts/test-store.sh passes an absolute path */
#endif

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

/* write n pages in one transaction: page pages[i] gets version vers[i] */
static int commit_many(fk_store *s, uint32_t b, const uint32_t *pages, const int *vers, uint32_t n, uint32_t size) {
  uint8_t p[FK_PAGE];
  if (fk_txn_begin(s, b)) return -1;
  for (uint32_t i = 0; i < n; i++) {
    page(p, vers[i]);
    if (i % 2) { /* every other page goes in through the ownership-transfer call */
      uint8_t *d = malloc(FK_PAGE);
      if (!d) { fk_txn_abort(s); return -1; }
      memcpy(d, p, FK_PAGE);
      if (fk_txn_write_owned(s, pages[i], d)) { free(d); fk_txn_abort(s); return -1; }
    } else if (fk_txn_write(s, pages[i], p)) { fk_txn_abort(s); return -1; }
  }
  return fk_txn_commit(s, size);
}

static int copy_file(const char *from, const char *to) {
  FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
  int ok = a && b;
  unsigned char buf[8192];
  size_t n;
  while (ok && (n = fread(buf, 1, sizeof buf, a)) > 0) ok = fwrite(buf, 1, n, b) == n;
  if (a) fclose(a);
  if (b && fclose(b) != 0) ok = 0;
  return ok;
}
static uint32_t file_version(const char *path) {
  unsigned char h[12];
  FILE *f = fopen(path, "rb");
  if (!f || fread(h, 1, sizeof h, f) != sizeof h) { if (f) fclose(f); return 0; }
  fclose(f);
  return (uint32_t)h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24;
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

  printf("== ownership-transfer write: the store takes the buffer on success, the caller keeps it on error\n");
  {
    char po[300];
    snprintf(po, sizeof po, "%s/owned.fk", dir);
    fk_io oio; fk_store *os = NULL;
    CHECK(open_path(po, &oio, &os), "open a store");
    uint8_t *d = malloc(FK_PAGE);
    page(d, 7);
    CHECK(fk_txn_write_owned(os, 0, d) == FK_EINVAL, "no transaction open: refused, the caller still owns the buffer (it is handed over again below)");
    CHECK(fk_txn_begin(os, 0) == 0 && fk_txn_write_owned(os, 0, NULL) == FK_EINVAL, "a NULL buffer is refused");
    CHECK(fk_txn_write_owned(os, 0, d) == 0, "accepted: the store owns the buffer now (it must not be touched again)");
    uint8_t *d2 = malloc(FK_PAGE);
    page(d2, 8);
    CHECK(fk_txn_write_owned(os, 1, d2) == 0 && fk_txn_commit(os, 2) == 0, "two pages handed over and committed");
    CHECK(has(os, 0, 0, 7) && has(os, 0, 1, 8), "both read back, and the store did not alias or free them early");
    uint8_t *d3 = malloc(FK_PAGE);
    page(d3, 9);
    CHECK(fk_txn_begin(os, 0) == 0 && fk_txn_write_owned(os, 0, d3) == 0, "a page handed over in a transaction that is then aborted");
    fk_txn_abort(os); /* frees d3: the address sanitizer reports a leak or a double free here if it does not */
    CHECK(has(os, 0, 0, 7), "the abort left the committed page alone");
    fk_close(os); fk_file_close(&oio);
    unlink(po);
  }
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
    CHECK(fk_fork(s, 1, &nf) == 0 && nf == 3 && has(s, 3, 0, 3000), "after compaction forking works, and the new branch gets the next never-used id (3)");
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

  printf("== sparse index: memory follows what a layer holds, not how high its page numbers go\n");
  {
    char p[300];
    snprintf(p, sizeof p, "%s/sparse.fk", dir);
    fk_io i3; fk_store *s3 = NULL;
    uint8_t z[FK_PAGE], pg[FK_PAGE];
    memset(z, 0, sizeof z);
    CHECK(open_path(p, &i3, &s3) && commit1(s3, 0, 0, 1, 1) == 0, "a store with one page");
    uint64_t base = fk_index_bytes(s3);
    uint32_t f1 = 0;
    CHECK(fk_fork(s3, 0, &f1) == 0 && commit1(s3, f1, 1000000, 77, 1000001) == 0, "a fork that writes a single page, number 1,000,000");
    CHECK(fk_index_bytes(s3) - base < 4096, "the index grew by less than 4 KiB (one 8-byte offset per page up to the highest would have been 8 MB)");
    CHECK(has(s3, f1, 1000000, 77) && has(s3, f1, 0, 1) && has(s3, 0, 0, 1), "the page reads back, next to the parent's page");
    CHECK(fk_read(s3, f1, 500000, pg) == 0 && !memcmp(pg, z, FK_PAGE), "pages in the gap read as zeros");
    uint64_t one = fk_index_bytes(s3);
    int ok = 1;
    for (uint32_t i = 0; i < 300 && ok; i++) {
      uint32_t f = 0;
      ok = fk_fork(s3, 0, &f) == 0 && commit1(s3, f, 900000 + i, 1000 + (int)i, 1000001) == 0;
    }
    CHECK(ok && fk_index_bytes(s3) - one < (1u << 20), "300 more forks, each writing one page near page 900,000: the index grew by less than 1 MiB in total");
    ok = 1;
    for (uint32_t i = 0; i < 300 && ok; i++) ok = has(s3, 2 + i, 900000 + i, 1000 + (int)i) && has(s3, 2 + i, 0, 1) && fk_read(s3, 2 + i, 1000000, pg) == 0 && !memcmp(pg, z, FK_PAGE);
    CHECK(ok, "every one of them reads back its page and the parent's page 0, and its last page (never written) reads as zeros");

    /* a shrink must not add an entry for every page number it removes, only for pages that exist below */
    uint64_t pre = fk_index_bytes(s3);
    CHECK(commit1(s3, f1, 1, 5, 2) == 0 && fk_index_bytes(s3) - pre < 4096, "shrinking the fork from 1,000,001 pages to 2: less than 4 KiB of index (no entry for the 999,998 pages that never existed)");
    CHECK(fk_read(s3, f1, 1000000, pg) == FK_ENOENT, "its old last page is gone");
    CHECK(commit1(s3, f1, 2, 6, 1000001) == 0 && fk_read(s3, f1, 1000000, pg) == 0 && !memcmp(pg, z, FK_PAGE) && has(s3, f1, 2, 6) && has(s3, f1, 1, 5), "growing it back: the removed page reads as zeros, not as 77");
    CHECK(has(s3, 0, 0, 1), "main is unaffected");
    fk_close(s3); fk_file_close(&i3);

    /* many pages in one layer: sequential, stride 1024, stride 65536. Every table growth must keep every entry. */
    snprintf(p, sizeof p, "%s/many.fk", dir);
    enum { NP = 3000 };
    static uint32_t pages[NP];
    static int vers[NP];
    uint32_t n = 0, top = 0;
    for (uint32_t i = 0; i < 1000; i++) pages[n++] = i;
    for (uint32_t i = 0; i < 1000; i++) pages[n++] = 100000 + i * 1024;
    for (uint32_t i = 0; i < 1000; i++) pages[n++] = 10000000 + i * 65536;
    for (uint32_t i = 0; i < n; i++) { vers[i] = 1 + (int)i; if (pages[i] > top) top = pages[i]; }
    top += 1;
    CHECK(open_path(p, &i3, &s3), "a new store");
    ok = 1;
    for (uint32_t i = 0; i < n && ok; i += 100) ok = commit_many(s3, 0, pages + i, vers + i, 100, top) == 0; /* 30 commits into one layer: the table grows 10 times */
    CHECK(ok, "3,000 pages in 30 commits into one layer");
    ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) ok = has(s3, 0, pages[i], vers[i]);
    CHECK(ok, "all 3,000 read back");
    CHECK(fk_index_bytes(s3) < (uint64_t)NP * 48 + 8192, "index memory is at most 48 bytes per page (table at least a quarter full)");
    for (uint32_t i = 0; i < n; i += 3) { vers[i] += 10000; }
    ok = 1;
    for (uint32_t i = 0; i < n && ok; i += 3) ok = commit_many(s3, 0, pages + i, vers + i, 1, top) == 0;
    CHECK(ok, "every third page overwritten (an overwrite must replace the entry)");
    ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) ok = has(s3, 0, pages[i], vers[i]);
    CHECK(ok, "all 3,000 read back, overwritten ones with their new content");
    fk_close(s3); fk_file_close(&i3);
    s3 = NULL;
    CHECK(open_path(p, &i3, &s3), "reopen (the index is rebuilt by replaying the log)");
    ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) ok = has(s3, 0, pages[i], vers[i]);
    CHECK(ok, "all 3,000 read back");
    CHECK(fk_compact(s3) == 0, "compact (the index is dumped to a snapshot, sorted by page)");
    ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) ok = has(s3, 0, pages[i], vers[i]);
    CHECK(ok, "all 3,000 read back");
    fk_close(s3); fk_file_close(&i3);
    s3 = NULL;
    CHECK(open_path(p, &i3, &s3), "reopen the compacted file (the snapshot is loaded into tables)");
    ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) ok = has(s3, 0, pages[i], vers[i]);
    CHECK(ok, "all 3,000 read back");
    fk_close(s3); fk_file_close(&i3);
  }

  printf("== branch ids: next-fit reuse, persistence, the limit\n");
  {
    char p[300];
    snprintf(p, sizeof p, "%s/ids.fk", dir);
    fk_io i4; fk_store *s4 = NULL;
    uint32_t a = 0, b = 0, c3 = 0, d = 0, e = 0, sz = 0;
    CHECK(open_path(p, &i4, &s4) && fk_fork(s4, 0, &a) == 0 && a == 1 && fk_fork(s4, 0, &b) == 0 && b == 2, "fork gives ids 1 and 2");
    CHECK(fk_discard(s4, 1) == 0 && !fk_branch_alive(s4, 1) && fk_size(s4, 1, &sz) == FK_ENOENT, "discard 1: the id is dead and reads return FK_ENOENT");
    CHECK(fk_fork(s4, 0, &c3) == 0 && c3 == 3, "the next fork gets 3, not the id just discarded (next fit)");
    fk_close(s4); fk_file_close(&i4);
    s4 = NULL;
    CHECK(open_path(p, &i4, &s4) && fk_branch_count(s4) == 4 && !fk_branch_alive(s4, 1) && fk_branch_alive(s4, 2) && fk_branch_alive(s4, 3), "reopen: the same branches are alive");
    CHECK(fk_fork(s4, 0, &d) == 0 && d == 4, "after a reopen the cursor is where it was (replayed from the log): the next fork gets 4");
    CHECK(fk_discard(s4, 2) == 0 && fk_discard(s4, 3) == 0 && fk_discard(s4, 4) == 0 && fk_compact(s4) == 0, "discard 2, 3, 4 and compact");
    fk_close(s4); fk_file_close(&i4);
    s4 = NULL;
    CHECK(open_path(p, &i4, &s4) && fk_fork(s4, 0, &e) == 0 && e == 5, "after compaction and a reopen the cursor is where it was (kept in the snapshot): the next fork gets 5, not the lowest free id 1");
    fk_close(s4); fk_file_close(&i4);

    /* all 1,024 ids in use, then reuse */
    snprintf(p, sizeof p, "%s/limit.fk", dir);
    s4 = NULL;
    CHECK(open_path(p, &i4, &s4) && commit1(s4, 0, 0, 1, 2) == 0 && commit1(s4, 0, 1, 42, 2) == 0, "a store whose main has p0 = 1, p1 = 42");
    int ok = 1;
    for (uint32_t i = 1; i < FK_MAX_BRANCHES && ok; i++) { uint32_t f = 0; ok = fk_fork(s4, 0, &f) == 0 && f == i; }
    CHECK(ok && fk_branch_count(s4) == FK_MAX_BRANCHES, "1,023 forks give ids 1..1023 in order");
    uint32_t extra = 0;
    CHECK(fk_fork(s4, 0, &extra) == FK_ELIMIT, "the 1,024th branch: FK_ELIMIT (the store is unchanged)");
    CHECK(commit1(s4, 500, 1, 7777, 2) == 0 && commit1(s4, 10, 1, 7778, 2) == 0 && commit1(s4, 20, 1, 7779, 2) == 0, "write p1 on branches 500, 10 and 20");
    CHECK(fk_discard(s4, 500) == 0 && !fk_branch_alive(s4, 500), "discard 500");
    uint32_t n500 = 0;
    CHECK(fk_fork(s4, 0, &n500) == 0 && n500 == 500, "the only free id is reused");
    CHECK(has(s4, 500, 0, 1) && has(s4, 500, 1, 42), "and the new branch reads main's p1 = 42, not 7777 from the branch that had the id");
    CHECK(fk_fork(s4, 0, &extra) == FK_ELIMIT, "full again");
    CHECK(fk_discard(s4, 20) == 0 && fk_discard(s4, 10) == 0, "discard 20, then 10");
    uint32_t n1 = 0, n2 = 0;
    CHECK(fk_fork(s4, 0, &n1) == 0 && n1 == 10 && fk_fork(s4, 0, &n2) == 0 && n2 == 20, "next fit from 500: wraps round, finds 10 first, then 20 (not 20 first, although it was discarded first)");
    CHECK(has(s4, 10, 1, 42) && has(s4, 20, 1, 42) && has(s4, 20, 0, 1), "both read main's pages, not the old 7778 and 7779");
    fk_close(s4); fk_file_close(&i4);
    s4 = NULL;
    CHECK(open_path(p, &i4, &s4) && fk_branch_count(s4) == FK_MAX_BRANCHES && fk_branch_alive(s4, 10) && fk_branch_alive(s4, 20) && fk_branch_alive(s4, 500), "reopen: replaying the log reproduces the reuse");
    CHECK(has(s4, 10, 1, 42) && has(s4, 20, 1, 42) && has(s4, 500, 1, 42) && fk_fork(s4, 0, &extra) == FK_ELIMIT, "same contents, still full");
    CHECK(fk_compact(s4) == 0 && has(s4, 500, 1, 42) && fk_discard(s4, 777) == 0 && fk_fork(s4, 0, &extra) == 0 && extra == 777, "after compaction (1,024 branches in the snapshot) the same");
    fk_close(s4); fk_file_close(&i4);
  }

  printf("== format versions: a version 2 file from the previous release, and a version from the future\n");
  {
    char src[400], p[300];
    snprintf(src, sizeof src, "%s/v2-compacted.fk", FK_TEST_DATA);
    snprintf(p, sizeof p, "%s/v2copy.fk", dir);
    fk_io i5; fk_store *s5 = NULL;
    CHECK(copy_file(src, p) && file_version(p) == 2, "a version 2 store with a snapshot, written by the store as of commit 7008c1c (test/data)");
    CHECK(open_path(p, &i5, &s5), "it opens");
    CHECK(s5 && fk_branch_count(s5) == 4 && fk_branch_alive(s5, 0) && !fk_branch_alive(s5, 1) && fk_branch_alive(s5, 2) && fk_branch_alive(s5, 3), "branches 0, 2, 3 are alive, 1 is not");
    CHECK(s5 && has(s5, 0, 0, 5) && has(s5, 0, 1, 2) && has(s5, 2, 0, 1) && has(s5, 2, 1, 33) && has(s5, 3, 0, 77) && has(s5, 3, 1, 33), "every branch reads back what it held");
    uint32_t n = 0;
    CHECK(s5 && fk_fork(s5, 0, &n) == 0 && n == 4 && commit1(s5, n, 1, 60, 2) == 0 && has(s5, n, 1, 60), "a fork works and gets the next new id (4)");
    CHECK(s5 && fk_discard(s5, 4) == 0 && fk_fork(s5, 0, &n) == 0 && n == 5, "version 2 files never reuse ids: after discarding 4 the next fork gets 5");
    if (s5) fk_close(s5);
    fk_file_close(&i5);
    CHECK(file_version(p) == 2, "the file is still version 2 (an older program can still read it)");
    s5 = NULL;
    CHECK(open_path(p, &i5, &s5) && fk_branch_count(s5) == 6 && fk_branch_alive(s5, 5) && !fk_branch_alive(s5, 4) && has(s5, 3, 0, 77), "reopen with the version 2 rules");
    CHECK(s5 && fk_compact(s5) == 0, "compact it");
    if (s5) fk_close(s5);
    fk_file_close(&i5);
    CHECK(file_version(p) == 3, "the compacted file is version 3");
    s5 = NULL;
    CHECK(open_path(p, &i5, &s5) && has(s5, 0, 0, 5) && has(s5, 2, 1, 33) && has(s5, 3, 0, 77) && fk_branch_alive(s5, 5) && !fk_branch_alive(s5, 4), "reopen: nothing lost");
    n = 0;
    CHECK(s5 && fk_fork(s5, 0, &n) == 0 && n == 6, "the cursor carried over from the version 2 history (latest fork was 5): the next fork gets 6");
    if (s5) fk_close(s5);
    fk_file_close(&i5);
    /* a version this program does not know is refused, and nothing is cut off */
    snprintf(p, sizeof p, "%s/future.fk", dir);
    CHECK(copy_file(src, p), "copy the fixture again");
    patch_header_version(p, 4);
    long before = fsz(p);
    s5 = NULL;
    CHECK(fk_file_open(p, &i5) == 0 && fk_open(&i5, &s5) == FK_ECORRUPT && fsz(p) == before, "relabelled as version 4: refused with FK_ECORRUPT, the file is untouched");
    fk_file_close(&i5);
  }

  printf("== layers: dead layers are freed and single-child layers are folded\n");
  {
    char p[300];
    snprintf(p, sizeof p, "%s/layers.fk", dir);
    fk_io i6; fk_store *s6 = NULL;
    uint8_t z[FK_PAGE], pg[FK_PAGE];
    memset(z, 0, sizeof z);
    int ok = 1;
    uint32_t depth = 0;

    /* 1. a fork-write-discard loop does not grow the tree */
    CHECK(open_path(p, &i6, &s6), "a new store");
    for (uint32_t i = 0; i < 40; i++) ok = ok && commit1(s6, 0, i, 1, 40) == 0;
    CHECK(ok && fk_layer_count(s6) == 1, "40 pages on main: one layer");
    uint32_t maxl = 0, maxd = 0;
    for (int it = 1; it <= 300 && ok; it++) {
      uint32_t f = 0;
      ok = commit1(s6, 0, (uint32_t)it % 40, 100 + it, 40) == 0 && fk_fork(s6, 0, &f) == 0 && commit1(s6, f, 39, 5000 + it, 40) == 0 && commit1(s6, f, 38, 6000 + it, 40) == 0 &&
           fk_discard(s6, f) == 0 && fk_branch_depth(s6, 0, &depth) == 0;
      if (fk_layer_count(s6) > maxl) maxl = fk_layer_count(s6);
      if (depth > maxd) maxd = depth;
    }
    printf("    (300 iterations: at most %u layers, main at most %u deep)\n", maxl, maxd);
    CHECK(ok && maxl <= 3 && maxd <= 2, "300 rounds of write, fork, write on the fork, discard it: at most 3 layers exist and main is at most 2 deep, at every step");
    ok = 1;
    for (uint32_t i = 1; i < 40 && ok; i++) {
      int v = 1;
      for (int it = 1; it <= 300; it++) if ((uint32_t)it % 40 == i) v = 100 + it;
      ok = has(s6, 0, i, v);
    }
    CHECK(ok && has(s6, 0, 39, 1) == 0 && fk_read(s6, 0, 0, pg) == 0, "main reads back what it wrote (and not what the discarded forks wrote)");
    fk_close(s6); fk_file_close(&i6);
    s6 = NULL;
    CHECK(open_path(p, &i6, &s6) && fk_layer_count(s6) <= 3, "reopen: the same few layers (the log is replayed through the same rules)");
    CHECK(fk_compact(s6) == 0 && fk_layer_count(s6) <= 3, "compact");
    ok = 1;
    for (uint32_t i = 1; i < 40 && ok; i++) {
      int v = 1;
      for (int it = 1; it <= 300; it++) if ((uint32_t)it % 40 == i) v = 100 + it;
      ok = has(s6, 0, i, v);
    }
    CHECK(ok, "main still reads back the same");
    fk_close(s6); fk_file_close(&i6);

    /* 2. discard in every kind of order; the layer array is no longer in creation order, compaction must not care */
    snprintf(p, sizeof p, "%s/layers2.fk", dir);
    s6 = NULL;
    CHECK(open_path(p, &i6, &s6) && commit1(s6, 0, 0, 1, 4) == 0 && commit1(s6, 0, 1, 2, 4) == 0, "a new store");
    uint32_t f[8] = {0};
    ok = 1;
    for (int i = 1; i <= 6 && ok; i++) { /* a tree of forks, some of forks, each with its own page */
      uint32_t src = i <= 3 ? 0 : f[i - 3];
      ok = fk_fork(s6, src, &f[i]) == 0 && commit1(s6, f[i], 2, 10 * i, 4) == 0 && commit1(s6, src, 3, 70 + i, 4) == 0;
    }
    CHECK(ok, "six forks (three of main, three of those), each writing page 2, with writes in between on the branch they came from");
    CHECK(fk_discard(s6, f[2]) == 0 && fk_discard(s6, f[4]) == 0, "discard the second and fourth forks (the middle of the tree)");
    CHECK(has(s6, f[1], 2, 10) && has(s6, f[3], 2, 30) && has(s6, f[5], 2, 50) && has(s6, f[6], 2, 60) && has(s6, 0, 0, 1) && has(s6, f[1], 0, 1), "the others read back their pages");
    CHECK(fk_compact(s6) == 0, "compact");
    CHECK(has(s6, f[1], 2, 10) && has(s6, f[3], 2, 30) && has(s6, f[5], 2, 50) && has(s6, f[6], 2, 60) && has(s6, 0, 1, 2), "the same");
    fk_close(s6); fk_file_close(&i6);
    s6 = NULL;
    CHECK(open_path(p, &i6, &s6) && has(s6, f[1], 2, 10) && has(s6, f[3], 2, 30) && has(s6, f[5], 2, 50) && has(s6, f[6], 2, 60), "reopen the compacted file: the same");
    CHECK(fk_discard(s6, f[6]) == 0 && fk_discard(s6, f[1]) == 0 && fk_discard(s6, f[5]) == 0 && fk_discard(s6, f[3]) == 0, "discard the rest");
    CHECK(fk_layer_count(s6) == 1 && fk_branch_depth(s6, 0, &depth) == 0 && depth == 1, "one layer is left, main's");
    CHECK(has(s6, 0, 0, 1) && has(s6, 0, 1, 2) && has(s6, 0, 3, 73), "main reads back all it has (page 3 as its last write, 73; the later writes were made on forks that are gone)");
    fk_close(s6); fk_file_close(&i6);

    /* 3. a fold with the parent the larger layer: the child's pages go into the parent, which takes over the branch */
    snprintf(p, sizeof p, "%s/layers3.fk", dir);
    s6 = NULL;
    uint32_t g = 0;
    CHECK(open_path(p, &i6, &s6), "a new store");
    ok = 1;
    for (uint32_t i = 0; i < 30 && ok; i++) ok = commit1(s6, 0, i, 1, 30) == 0;
    ok = ok && fk_fork(s6, 0, &g) == 0 && commit1(s6, 0, 5, 9, 30) == 0 && fk_discard(s6, g) == 0;
    CHECK(ok && fk_layer_count(s6) == 1 && has(s6, 0, 5, 9) && has(s6, 0, 6, 1), "30 pages, fork, one write on main, discard the fork: one layer, the write wins over the page below it");
    CHECK(commit1(s6, 0, 7, 11, 30) == 0 && has(s6, 0, 7, 11) && has(s6, 0, 5, 9), "main keeps working after its layer was replaced");
    fk_close(s6); fk_file_close(&i6);

    /* 4. a fold with the child the larger layer: the parent's pages go into the child */
    snprintf(p, sizeof p, "%s/layers4.fk", dir);
    s6 = NULL;
    CHECK(open_path(p, &i6, &s6) && commit1(s6, 0, 0, 1, 2) == 0 && commit1(s6, 0, 1, 2, 2) == 0 && fk_fork(s6, 0, &g) == 0, "a store with two pages and a fork");
    ok = 1;
    for (uint32_t i = 0; i < 60 && ok; i++) ok = commit1(s6, 0, i == 0 ? 0 : i + 1, 50 + (int)i, 62) == 0; /* page 0 again, then pages 2..60 */
    CHECK(ok && fk_discard(s6, g) == 0 && fk_layer_count(s6) == 1, "main writes 60 more pages, the fork is discarded: one layer");
    CHECK(has(s6, 0, 0, 50) && has(s6, 0, 1, 2) && has(s6, 0, 2, 51), "page 0 is main's newer write, page 1 comes from the layer that was folded in");
    fk_close(s6); fk_file_close(&i6);

    /* 5. tombstones and sizes survive a fold, in both directions */
    for (int dir_a = 0; dir_a < 2; dir_a++) {
      snprintf(p, sizeof p, "%s/layers5%d.fk", dir, dir_a);
      s6 = NULL;
      uint32_t a = 0, b = 0;
      CHECK(open_path(p, &i6, &s6), "a new store");
      ok = 1;
      for (uint32_t i = 0; i < 10 && ok; i++) ok = commit1(s6, 0, i, 1, 10) == 0; /* the root layer */
      ok = ok && fk_fork(s6, 0, &a) == 0;
      uint32_t tombs = dir_a ? 6 : 2; /* a shrinks by 6 pages (more tombstones than the later layer has entries: it is the bigger) or by 2 */
      ok = ok && commit1(s6, a, 0, 2, 10 - tombs) == 0;
      ok = ok && fk_fork(s6, a, &b) == 0; /* freezes a's layer */
      uint32_t nw = dir_a ? 1 : 5; /* then a grows back and writes 1 page (smaller layer) or 5 */
      for (uint32_t i = 0; i < nw && ok; i++) ok = commit1(s6, a, i, 20 + (int)i, 10) == 0;
      ok = ok && fk_discard(s6, b) == 0;
      CHECK(ok && fk_layer_count(s6) == 3, dir_a ? "shrink by 6, fork, grow back writing 1 page, discard the fork (the tombstone layer is the bigger): 3 layers left (the root, main's head, and the fork's two merged)" : "shrink by 2, fork, grow back writing 5 pages, discard the fork (the later layer is the bigger): 3 layers left");
      int good = 1;
      for (uint32_t q = 10 - tombs; q < 10; q++) { /* pages removed by the shrink and not rewritten: zeros, not the root's data */
        int written = 0;
        for (uint32_t i = 0; i < nw; i++) if (q == i) written = 1;
        if (written) continue;
        good = good && fk_read(s6, a, q, pg) == 0 && !memcmp(pg, z, FK_PAGE);
      }
      CHECK(good, "pages removed by the shrink and not written since read as zeros");
      CHECK(has(s6, a, 0, 20) && has(s6, a, 9, 1) == 0 && fk_size(s6, a, &depth) == 0 && depth == 10, "the later write wins, and the size is the later layer's (10)");
      CHECK(has(s6, 0, 9, 1) && has(s6, 0, 0, 1), "main is unaffected");
      fk_close(s6); fk_file_close(&i6);
      s6 = NULL;
      CHECK(open_path(p, &i6, &s6) && fk_read(s6, a, 9, pg) == 0 && !memcmp(pg, z, FK_PAGE) && has(s6, a, 0, 20), "reopen: the same");
      fk_close(s6); fk_file_close(&i6);
    }
  }

  printf("\nTOTAL FAILS=%d\n", fails);
  char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)!system(cmd);
  return fails ? 1 : 0;
}
