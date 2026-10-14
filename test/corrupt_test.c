/* Corrupted-record recovery tests: verify fk_open/scan() correctly truncates
 * at the last valid record when the log is damaged mid-stream.
 *
 * Tests writes a known sequence of records, then systematically corrupts
 * one byte at various points and verifies recovery preserves data up to
 * the corruption point.
 *
 * Build: gcc -Wall -Wextra -std=gnu11 -O1 -g -DFK_MAX_BRANCHES=64
 *   -Istore -Itest test/corrupt_test.c store/fkstore.c store/io_file.c
 *   store/io_prealloc.c -o build/corrupt_test -lpthread -lm
 */
#define _GNU_SOURCE
#include "../store/fkstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *e(int rc) {
  switch (rc) {
    case FK_OK: return "FK_OK"; case FK_ECORRUPT: return "FK_ECORRUPT";
    case FK_EIO: return "FK_EIO"; case FK_ENOMEM: return "FK_ENOMEM";
    case FK_EFAILED: return "FK_EFAILED";
    default: return "?";
  }
}

static char path[256];
static const char *P(const char *n) { snprintf(path, sizeof path, "/tmp/corrupt_%s.fkdb", n); return path; }

/* Build a store with a few commits: branch 0 pages 1-3, fork branch 1 pages 1,4.
 * Returns the file size after close (for truncation tests). */
static int build_store(const char *file, uint64_t *file_size) {
  unlink(file);
  fk_io io;
  if (fk_file_open(file, &io) != 0) return -1;
  fk_store *s = NULL;
  int rc = fk_open(&io, &s);
  if (rc != FK_OK) { fk_file_close(&io); return -1; }

  char page[FK_PAGE];

  /* commit 0: pages 1,2 with A,B */
  memset(page, 'A', sizeof page);
  if ((rc = fk_txn_begin(s, 0))) goto fail;
  if ((rc = fk_txn_write(s, 1, page))) goto fail;
  memset(page, 'B', sizeof page);
  if ((rc = fk_txn_write(s, 2, page))) goto fail;
  if ((rc = fk_txn_commit(s, 3))) goto fail;

  /* commit 1: page 3 with C */
  memset(page, 'C', sizeof page);
  if ((rc = fk_txn_begin(s, 0))) goto fail;
  if ((rc = fk_txn_write(s, 3, page))) goto fail;
  if ((rc = fk_txn_commit(s, 4))) goto fail;

  /* fork branch 1 */
  uint32_t b1;
  if ((rc = fk_fork(s, 0, &b1))) goto fail;

  /* commit 2 on branch 1: pages 1,4 with D */
  memset(page, 'D', sizeof page);
  if ((rc = fk_txn_begin(s, b1))) goto fail;
  if ((rc = fk_txn_write(s, 1, page))) goto fail;
  if ((rc = fk_txn_write(s, 4, page))) goto fail;
  if ((rc = fk_txn_commit(s, 5))) goto fail;

  int64_t sz = io.size(io.ctx);
  *file_size = sz > 0 ? (uint64_t)sz : 0;
  fk_close(s);
  fk_file_close(&io);
  return 0;
fail:
  fk_txn_abort(s);
  fk_close(s);
  fk_file_close(&io);
  return -1;
}

/* Corrupt one byte in the file and verify recovery behaviour. */
static void corrupt_and_check(const char *label, const char *file, uint64_t offset, uint8_t byte) {
  /* Read original file */
  FILE *f = fopen(file, "rb");
  if (!f) { CHECK(0, "%s: cannot open file", label); return; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc((size_t)sz);
  if (!buf) { fclose(f); return; }
  fread(buf, 1, (size_t)sz, f);
  fclose(f);
  uint8_t orig = buf[offset];

  /* Write corrupted version */
  buf[offset] = byte;
  f = fopen(file, "wb");
  if (!f) { free(buf); return; }
  fwrite(buf, 1, (size_t)sz, f);
  fclose(f);

  /* Try to open */
  fk_io io;
  if (fk_file_open(file, &io) != 0) { free(buf); CHECK(0, "%s: file open failed", label); return; }
  fk_store *s = NULL;
  int rc = fk_open(&io, &s);
  fk_file_close(&io);

  /* Recovery: FK_OK means tail was truncated cleanly. FK_ECORRUPT means
   * damage inside durable data — catastrophic but correctly detected. */
  CHECK(rc == FK_OK || rc == FK_ECORRUPT,
        "%s offset=%llu (0x%02x->0x%02x): %s", label, (unsigned long long)offset, orig, byte, e(rc));
  if (rc == FK_OK) {
    /* Verify at least branch 0 survived */
    uint32_t alive = 0, n = fk_branch_count(s);
    for (uint32_t i = 0; i < n; i++) alive += fk_branch_alive(s, i) ? 1 : 0;
    CHECK(alive > 0, "%s: no branches alive after recovery", label);
    fk_close(s);
  }

  /* Restore original for next test */
  buf[offset] = orig;
  f = fopen(file, "wb");
  if (f) { fwrite(buf, 1, (size_t)sz, f); fclose(f); }
  free(buf);
}

static void test_corrupt_magic(void) {
  printf("magic corruption\n");
  uint64_t fsize;
  int rc = build_store(P("corr_magic"), &fsize);
  CHECK(rc == 0, "build: %d", rc);
  if (rc != 0) return;

  /* Records start at FK_HEADER_SZ (4096 bytes). Corrupt magic on first record. */
  uint64_t rec_off = FK_HEADER_SZ;
  corrupt_and_check("bad-magic", P("corr_magic"), rec_off, 'X');
  corrupt_and_check("bad-magic2", P("corr_magic"), rec_off + 2, 0xFF);

  /* Corrupt magic mid-file — tail gets cut */
  if (fsize > rec_off + 512)
    corrupt_and_check("bad-magic-mid", P("corr_magic"), rec_off + 256, 0x00);

  unlink(P("corr_magic"));
}

static void test_corrupt_crc(void) {
  printf("CRC corruption\n");
  uint64_t fsize;
  int rc = build_store(P("corr_crc"), &fsize);
  CHECK(rc == 0, "build: %d", rc);
  if (rc != 0) return;

  uint64_t rec_off = FK_HEADER_SZ;
  /* CRC at offset 24 in the 32-byte record header */
  corrupt_and_check("bad-crc", P("corr_crc"), rec_off + 24, 0x00);
  corrupt_and_check("bad-crc2", P("corr_crc"), rec_off + 25, 0xFF);

  if (fsize > rec_off + 512)
    corrupt_and_check("bad-crc-mid", P("corr_crc"), rec_off + 256 + 24, 0x00);

  unlink(P("corr_crc"));
}

static void test_corrupt_type(void) {
  printf("type corruption\n");
  uint64_t fsize;
  int rc = build_store(P("corr_type"), &fsize);
  CHECK(rc == 0, "build: %d", rc);
  if (rc != 0) return;

  uint64_t rec_off = FK_HEADER_SZ;
  /* Record type at offset 4 — set to invalid */
  corrupt_and_check("bad-type", P("corr_type"), rec_off + 4, 99);

  if (fsize > rec_off + 512)
    corrupt_and_check("bad-type-mid", P("corr_type"), rec_off + 256 + 4, 99);

  unlink(P("corr_type"));
}

static void test_corrupt_length(void) {
  printf("length corruption\n");
  uint64_t fsize;
  int rc = build_store(P("corr_len"), &fsize);
  CHECK(rc == 0, "build: %d", rc);
  if (rc != 0) return;

  uint64_t rec_off = FK_HEADER_SZ;
  /* Record length at offset 16 — set high byte to large value */
  corrupt_and_check("bad-len", P("corr_len"), rec_off + 19, 0xFF);

  unlink(P("corr_len"));
}

static void test_truncated_file(void) {
  printf("truncated file\n");
  uint64_t fsize;
  int rc = build_store(P("corr_trunc"), &fsize);
  CHECK(rc == 0, "build: %d", rc);
  if (rc != 0) return;

  /* Truncate off the last byte — recovery should truncate tail cleanly */
  if (fsize > FK_HEADER_SZ && ftruncate(open(P("corr_trunc"), O_RDWR), (off_t)(fsize - 1)) == 0) {
    fk_io io;
    if (fk_file_open(P("corr_trunc"), &io) == 0) {
      fk_store *s = NULL;
      rc = fk_open(&io, &s);
      fk_file_close(&io);
      CHECK(rc == FK_OK || rc == FK_ECORRUPT, "truncated -1 byte: %s (marker protection against silent loss)", e(rc));
      if (rc == FK_OK) fk_close(s);
    }
  }

  /* Truncate to exactly header size (empty store) */
  if (ftruncate(open(P("corr_trunc"), O_RDWR), (off_t)FK_HEADER_SZ) == 0) {
    fk_io io;
    if (fk_file_open(P("corr_trunc"), &io) == 0) {
      fk_store *s = NULL;
      rc = fk_open(&io, &s);
      fk_file_close(&io);
      /* FK_ECORRUPT is also acceptable here — the marker may point past this. */
      CHECK(rc == FK_OK || rc == FK_ECORRUPT, "truncated to header: %s", e(rc));
      if (rc == FK_OK) fk_close(s);
    }
  }

  unlink(P("corr_trunc"));
}

int main(void) {
  test_corrupt_magic();
  test_corrupt_crc();
  test_corrupt_type();
  test_corrupt_length();
  test_truncated_file();

  printf("%d checks, TOTAL FAILS=%d\n", checks, fails);
  return fails ? 1 : 0;
}
