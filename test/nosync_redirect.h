/* Force-included (-include) into store/io_file.c only, by the nosync test: every fsync and fdatasync call in that file goes to a counting function in
 * test/nosync_test.c instead of the C library. Plain preprocessor renames, so it needs no linker feature (GNU ld's --wrap does not exist on macOS), and it runs
 * after <unistd.h> has been included once, so the system prototypes keep their own names and aliases. */
#ifndef FK_NOSYNC_REDIRECT_H
#define FK_NOSYNC_REDIRECT_H
#include <unistd.h>
int fk_test_fsync(int fd);
#define fsync fk_test_fsync
#ifndef __APPLE__ /* io_file.c uses fsync alone on macOS */
int fk_test_fdatasync(int fd);
#define fdatasync fk_test_fdatasync
#endif
#endif
