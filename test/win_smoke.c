/* Minimal Windows smoke test for the I/O backend.
 * Creates a store file, writes a page, reads it back, closes cleanly.
 * Validates that fk_file_open/close and fk_store open/read/write/close
 * work through io_file_win.c on Windows.
 *
 * Build: gcc -Wall -Wextra -std=gnu11 -O2 -DFK_MAX_BRANCHES=64
 *   -Istore test/win_smoke.c store/fkstore.c store/io_file.c
 *   store/io_file_win.c store/io_prealloc.c -o build/win_smoke.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../store/fkstore.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; int ok_ = !!(c); printf("  %s %s\n", ok_ ? "PASS" : "FAIL", msg); if (!ok_) fails++; } while (0)

int main(void) {
  const char *path = "smoke_test.fkdb";
  fk_io io;

  /* Create a new store file */
  int rc = fk_file_open(path, &io);
  CHECK(rc == 0, "open file");

  fk_store *s = NULL;
  rc = fk_open(&io, &s);
  CHECK(rc == 0 && s != NULL, "open store");

  /* Write and commit a page */
  char page[FK_PAGE];
  memset(page, 'X', sizeof page);
  rc = fk_txn_begin(s, 0);
  CHECK(rc == 0, "begin txn");
  rc = fk_txn_write(s, 1, page);
  CHECK(rc == 0, "write page 1");
  rc = fk_txn_commit(s, 2);
  CHECK(rc == 0, "commit");

  /* Read it back */
  char buf[FK_PAGE];
  memset(buf, 0, sizeof buf);
  rc = fk_read(s, 0, 1, buf);
  CHECK(rc == 0 && memcmp(buf, page, FK_PAGE) == 0, "read back page 1");

  /* Fork a branch and write to it */
  uint32_t kid;
  rc = fk_fork(s, 0, &kid);
  CHECK(rc == 0, "fork");

  memset(page, 'Y', sizeof page);
  rc = fk_txn_begin(s, kid);
  CHECK(rc == 0, "begin txn on fork");
  rc = fk_txn_write(s, 1, page);
  CHECK(rc == 0, "write page 1 on fork");
  rc = fk_txn_commit(s, 2);
  CHECK(rc == 0, "commit on fork");

  /* Forked branch sees its own data */
  memset(buf, 0, sizeof buf);
  rc = fk_read(s, kid, 1, buf);
  CHECK(rc == 0 && memcmp(buf, page, FK_PAGE) == 0, "fork reads its page");

  /* Main branch still sees old data */
  memset(buf, 0, sizeof buf);
  memset(page, 'X', sizeof page);
  rc = fk_read(s, 0, 1, buf);
  CHECK(rc == 0 && memcmp(buf, page, FK_PAGE) == 0, "main reads its page");

  fk_close(s);
  fk_file_close(&io);

  /* Reopen and verify persistence */
  rc = fk_file_open(path, &io);
  CHECK(rc == 0, "reopen file");
  rc = fk_open(&io, &s);
  CHECK(rc == 0, "reopen store");

  memset(buf, 0, sizeof buf);
  memset(page, 'X', sizeof page);
  rc = fk_read(s, 0, 1, buf);
  CHECK(rc == 0 && memcmp(buf, page, FK_PAGE) == 0, "reopen: main page intact");

  memset(buf, 0, sizeof buf);
  memset(page, 'Y', sizeof page);
  rc = fk_read(s, kid, 1, buf);
  CHECK(rc == 0 && memcmp(buf, page, FK_PAGE) == 0, "reopen: fork page intact");

  fk_close(s);
  fk_file_close(&io);
  remove(path);

  printf("%d checks, TOTAL FAILS=%d\n", checks, fails);
  return fails ? 1 : 0;
}
