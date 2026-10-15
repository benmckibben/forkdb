/* Force-included (-include) into store/io_file_win.c only, by the Windows build of test/compact_smoke.c with -DFK_COUNT_FLUSH: every FlushFileBuffers call in that file
 * goes to a counting function in the test. A plain macro, so the test file itself (which does not include this header) still calls the real function. */
#ifndef FK_WIN_FLUSH_REDIRECT_H
#define FK_WIN_FLUSH_REDIRECT_H
#include <windows.h>
int fk_test_flush(HANDLE h);
#define FlushFileBuffers fk_test_flush
#endif
