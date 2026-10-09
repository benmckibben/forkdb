/* io_file.c: fk_io on a real file (pread/pwrite/fsync). */
#include "fkstore.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int64_t f_size(void *ctx) {
  struct stat st;
  if (fstat(*(int *)ctx, &st) != 0) return -1;
  return (int64_t)st.st_size;
}
static int f_pread(void *ctx, uint64_t off, void *buf, size_t n) {
  size_t done = 0;
  while (done < n) {
    ssize_t r = pread(*(int *)ctx, (char *)buf + done, n - done, (off_t)(off + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) break;
    done += (size_t)r;
  }
  return (int)done;
}
static int f_pwrite(void *ctx, uint64_t off, const void *buf, size_t n) {
  size_t done = 0;
  while (done < n) {
    ssize_t r = pwrite(*(int *)ctx, (const char *)buf + done, n - done, (off_t)(off + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    done += (size_t)r;
  }
  return 0;
}
static int f_truncate(void *ctx, uint64_t size) { return ftruncate(*(int *)ctx, (off_t)size); }
static int f_fsync(void *ctx) { return fdatasync(*(int *)ctx); }

int fk_file_open(const char *path, fk_io *io) {
  int existed = access(path, F_OK) == 0;
  int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return -1;
  int *p = malloc(sizeof *p);
  if (!p) { close(fd); return -1; }
  *p = fd;
  io->ctx = p;
  io->size = f_size;
  io->pread = f_pread;
  io->pwrite = f_pwrite;
  io->truncate = f_truncate;
  io->fsync = f_fsync;
  if (!existed) {
    /* Make the new directory entry durable too; a power cut must not forget the file itself. */
    char dir[4096];
    size_t n = 0;
    while (path[n] && n < sizeof dir - 1) { dir[n] = path[n]; n++; }
    dir[n] = 0;
    while (n > 0 && dir[n - 1] != '/') n--;
    if (n == 0) { dir[0] = '.'; dir[1] = 0; } else if (n > 1) dir[n - 1] = 0; else dir[1] = 0;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { (void)fsync(dfd); close(dfd); }
  }
  return 0;
}
void fk_file_close(fk_io *io) {
  if (io->ctx) { close(*(int *)io->ctx); free(io->ctx); io->ctx = NULL; }
}
