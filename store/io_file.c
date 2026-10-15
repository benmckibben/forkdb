/* io_file.c: fk_io on a real file (pread/pwrite/fsync), with the sibling operations used by compaction.
 * On Windows (_WIN32) this delegates to io_file_win.c. */
#define _GNU_SOURCE
#include "fkstore.h"

#ifdef _WIN32
/* io_file_win.c provides fk_file_open and fk_file_close — nothing else needed here */
#else

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef OOM_INJECT
#include "oom_harness.h" /* after the system headers; shares oom_counter with the test */
#endif

typedef struct { int fd; char *path; int nosync; } FC;
#define FD(ctx) (((FC *)(ctx))->fd)

static int64_t f_size(void *ctx) {
  struct stat st;
  if (fstat(FD(ctx), &st) != 0) return -1;
  return (int64_t)st.st_size;
}
static int f_pread(void *ctx, uint64_t off, void *buf, size_t n) {
  size_t done = 0;
  while (done < n) {
    ssize_t r = pread(FD(ctx), (char *)buf + done, n - done, (off_t)(off + done));
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
    ssize_t r = pwrite(FD(ctx), (const char *)buf + done, n - done, (off_t)(off + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    done += (size_t)r;
  }
  return 0;
}
static int f_truncate(void *ctx, uint64_t size) { return ftruncate(FD(ctx), (off_t)size); }
static int f_fsync(void *ctx) {
#ifndef FK_BUG_NOSYNC_STILL_SYNCS
  if (((FC *)ctx)->nosync) return 0;
#endif
#ifdef __APPLE__
  return fsync(FD(ctx));
#else
  return fdatasync(FD(ctx));
#endif
}

static int sync_dir_of(const char *path) {
  char dir[4096];
  size_t n = 0;
  while (path[n] && n < sizeof dir - 1) { dir[n] = path[n]; n++; }
  dir[n] = 0;
  while (n > 0 && dir[n - 1] != '/') n--;
  if (n == 0) { dir[0] = '.'; dir[1] = 0; } else if (n > 1) dir[n - 1] = 0; else dir[1] = 0;
  int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0) return -1;
  int rc = fsync(dfd);
  close(dfd);
  return rc;
}
static char *tmp_path(const char *p) {
  size_t n = strlen(p);
  char *t = malloc(n + 9);
  if (!t) return NULL;
  memcpy(t, p, n);
  memcpy(t + n, ".compact", 9);
  return t;
}

static int f_sibling_create(void *ctx, fk_io *tmp);
static int f_sibling_rename(void *ctx, fk_io *tmp);
static int f_sync_dir(void *ctx) {
#ifndef FK_BUG_NOSYNC_STILL_SYNCS
  if (((FC *)ctx)->nosync) return 0;
#endif
  return sync_dir_of(((FC *)ctx)->path);
}
static void f_sibling_remove(void *ctx, fk_io *tmp);

static void fill(fk_io *io, FC *c, int main_file) {
  memset(io, 0, sizeof *io);
  io->ctx = c;
  io->size = f_size;
  io->pread = f_pread;
  io->pwrite = f_pwrite;
  io->truncate = f_truncate;
  io->fsync = f_fsync;
  if (main_file) {
    io->sibling_create = f_sibling_create;
    io->sibling_rename = f_sibling_rename;
    io->sync_dir = f_sync_dir;
    io->sibling_remove = f_sibling_remove;
  }
}

static int f_sibling_create(void *ctx, fk_io *tmp) {
  FC *m = ctx;
  char *tp = tmp_path(m->path);
  if (!tp) return -1;
  int fd = open(tp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  FC *t = fd >= 0 ? malloc(sizeof *t) : NULL;
  if (!t) {
    if (fd >= 0) { close(fd); unlink(tp); }
    free(tp);
    return -1;
  }
  t->fd = fd;
  t->path = tp;
  t->nosync = m->nosync;
  fill(tmp, t, 0);
  return 0;
}
static int f_sibling_rename(void *ctx, fk_io *tmp) {
  FC *m = ctx, *t = tmp->ctx;
  if (rename(t->path, m->path) != 0) return -1; /* nothing changed */
  close(m->fd);
  m->fd = t->fd;
  free(t->path);
  free(t);
  tmp->ctx = NULL;
  return 0;
}
static void f_sibling_remove(void *ctx, fk_io *tmp) {
  FC *m = ctx;
  if (tmp && tmp->ctx) {
    FC *t = tmp->ctx;
    close(t->fd);
    unlink(t->path);
    free(t->path);
    free(t);
    tmp->ctx = NULL;
    return;
  }
  char *tp = tmp_path(m->path);
  if (tp) { unlink(tp); free(tp); }
}

int fk_file_open(const char *path, fk_io *io) { return fk_file_open_ex(path, io, 0); }

int fk_file_open_ex(const char *path, fk_io *io, int flags) {
  int existed = access(path, F_OK) == 0;
  int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return -1;
#ifndef FK_BUG_NO_FILE_LOCK
  /* See fk_file_open in fkstore.h. EWOULDBLOCK is a proven conflict; any other failure to lock (ENOLCK, EOPNOTSUPP) is not. */
  if (flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) { close(fd); return FK_ELOCKED; }
#endif
  FC *c = malloc(sizeof *c);
  char *p = strdup(path);
  if (!c || !p) { free(c); free(p); close(fd); return -1; }
  c->fd = fd;
  c->path = p;
  c->nosync = (flags & FK_FILE_NOSYNC) != 0;
  fill(io, c, 1);
  /* Make a new directory entry durable too; a power cut must not forget the file itself. */
  if (!existed && !c->nosync) (void)sync_dir_of(path);
  return 0;
}
void fk_file_close(fk_io *io) {
  if (io->ctx) {
    FC *c = io->ctx;
    close(c->fd);
    free(c->path);
    free(c);
    io->ctx = NULL;
  }
}
#endif /* _WIN32 */
