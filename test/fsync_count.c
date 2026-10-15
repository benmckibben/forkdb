/* LD_PRELOAD interposer that counts fsync and fdatasync calls.
 * Built: gcc -shared -fPIC -O2 -o build/ext/fsync_count.so test/fsync_count.c -ldl
 * Use: LD_PRELOAD=<path> FK_FSYNC_COUNT_FILE=<tempfile> <program>
 * On exit writes "FSYNC_COUNT <n>" to FK_FSYNC_COUNT_FILE. */

#define _GNU_SOURCE
#include <unistd.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

static atomic_int counter;

static int (*real_fsync)(int) = NULL;
static int (*real_fdatasync)(int) = NULL;

__attribute__((constructor))
static void init_real(void) {
    real_fsync = dlsym(RTLD_NEXT, "fsync");
    real_fdatasync = dlsym(RTLD_NEXT, "fdatasync");
}

int fsync(int fd) {
    if (!real_fsync) real_fsync = dlsym(RTLD_NEXT, "fsync");
    atomic_fetch_add(&counter, 1);
    return real_fsync ? real_fsync(fd) : -1;
}

int fdatasync(int fd) {
    if (!real_fdatasync) real_fdatasync = dlsym(RTLD_NEXT, "fdatasync");
    atomic_fetch_add(&counter, 1);
    return real_fdatasync ? real_fdatasync(fd) : -1;
}

static void write_count_file(void) {
    const char *path = getenv("FK_FSYNC_COUNT_FILE");
    if (!path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[64];
    int n = snprintf(buf, sizeof buf, "FSYNC_COUNT %d\n", (int)atomic_load(&counter));
    if (n > 0) {
        ssize_t w = write(fd, buf, (size_t)n);
        (void)w;
    }
    close(fd);
}

__attribute__((destructor))
static void on_unload(void) {
    write_count_file();
}
