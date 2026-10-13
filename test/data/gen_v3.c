/* gen_v3.c: writes test/data/v3-compacted.fk, a format-version-3 store with a snapshot, using the store as it was at commit 1e59d6c
 * (before format version 4). See README.md in this directory. The page contents are page(v)[i] = (uint8_t)(v * 31 + i), as in file_test.c.
 *
 * History: main p0=1 p1=2; fork 1, p0=9; fork 2 (of main), no change (so it shares main's content); fork 3 (of 1), p1=33;
 *          discard 2; COMPACT (version 3 snapshot, with branch ids 0, 1, 3 alive and 2 dead);
 *          then main p0=5; fork 4 (of 3), p0=77 (id 2 is reused by the next fork after the cursor: the latest fork was 4, so the next is 5).
 * Resulting state: ids 0, 1, 3, 4 alive, id 2 dead, branch_count 5, allocation cursor 4.
 *   main = (5, 2); branch 1 = (9, 2); branch 3 = (9, 33); branch 4 = (77, 33).
 * Four live branches with four different contents, so that a loader that gave them all the same state id would be caught. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fkstore.h"

static int commit1(fk_store *s, uint32_t b, uint32_t pg, int v, uint32_t size) {
  uint8_t p[FK_PAGE];
  for (int i = 0; i < (int)FK_PAGE; i++) p[i] = (uint8_t)(v * 31 + i);
  return fk_txn_begin(s, b) || fk_txn_write(s, pg, p) || fk_txn_commit(s, size);
}
int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr, "usage: gen_v3 <output file>\n"); return 2; }
  fk_io io; fk_store *s = NULL; uint32_t f1 = 0, f2 = 0, f3 = 0, f4 = 0;
  remove(argv[1]);
  if (fk_file_open(argv[1], &io) || fk_open(&io, &s)) return 1;
  if (commit1(s, 0, 0, 1, 2) || commit1(s, 0, 1, 2, 2)) return 1;
  if (fk_fork(s, 0, &f1) || commit1(s, f1, 0, 9, 2)) return 1;
  if (fk_fork(s, 0, &f2)) return 1;
  if (fk_fork(s, f1, &f3) || commit1(s, f3, 1, 33, 2)) return 1;
  if (fk_discard(s, f2)) return 1;
  if (fk_compact(s)) return 1;
  if (commit1(s, 0, 0, 5, 2)) return 1;
  if (fk_fork(s, f3, &f4) || commit1(s, f4, 0, 77, 2)) return 1;
  fk_close(s); fk_file_close(&io);
  return f1 == 1 && f2 == 2 && f3 == 3 && f4 == 4 ? 0 : 1;
}
