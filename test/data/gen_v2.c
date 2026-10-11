/* gen_v2.c: writes test/data/v2-compacted.fk, a format-version-2 store with a snapshot, using the store as it was at commit 7008c1c
 * (before format version 3). See README.md in this directory. The page contents are page(v)[i] = (uint8_t)(v * 31 + i), as in file_test.c.
 *
 * History: main p0=1 p1=2; fork 1, p0=9; fork 2, p1=33; discard 1; COMPACT (version 2 snapshot);
 *          then main p0=5; fork 3 (of branch 2), p0=77.
 * Resulting state: ids 0, 2, 3 alive, id 1 dead, branch_count 4. main = (5, 2); branch 2 = (1, 33); branch 3 = (77, 33). */
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
  if (argc != 2) { fprintf(stderr, "usage: gen_v2 <output file>\n"); return 2; }
  fk_io io; fk_store *s = NULL; uint32_t f1 = 0, f2 = 0, f3 = 0;
  remove(argv[1]);
  if (fk_file_open(argv[1], &io) || fk_open(&io, &s)) return 1;
  if (commit1(s, 0, 0, 1, 2) || commit1(s, 0, 1, 2, 2)) return 1;
  if (fk_fork(s, 0, &f1) || commit1(s, f1, 0, 9, 2)) return 1;
  if (fk_fork(s, 0, &f2) || commit1(s, f2, 1, 33, 2)) return 1;
  if (fk_discard(s, f1)) return 1;
  if (fk_compact(s)) return 1;
  if (commit1(s, 0, 0, 5, 2)) return 1;
  if (fk_fork(s, f2, &f3) || commit1(s, f3, 0, 77, 2)) return 1;
  fk_close(s); fk_file_close(&io);
  return f1 == 1 && f2 == 2 && f3 == 3 ? 0 : 1;
}
