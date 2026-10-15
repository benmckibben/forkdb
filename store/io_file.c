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
static unsigned long long g_syncs; /* see fk_file_sync_count in fkstore.h */
unsigned long long fk_file_sync_count(void) { return __atomic_load_n(&g_syncs, __ATOMIC_RELAXED); }
#define COUNT_SYNC() ((void)__atomic_fetch_add(&g_syncs, 1, __ATOMIC_RELAXED))
static int f_fsync(void *ctx) {
#ifndef FK_BUG_NOSYNC_STILL_SYNCS
  if (((FC *)ctx)->nosync) return 0;
#endif
  COUNT_SYNC();
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
  COUNT_SYNC();
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
  /* The replacement must never be more open than the file it replaces: created 0600, then given the original's mode (and, where the system lets this process, its owner and
   * group). A store its owner had made 0600 used to come out of a compaction 0644. Extended attributes and ACLs are not carried over. */
  struct stat mst;
  int have_mode = fstat(m->fd, &mst) == 0;
  int fd = open(tp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
#ifdef FK_BUG_COMPACT_WIDENS_MODE
  (void)have_mode;
  if (fd >= 0) (void)fchmod(fd, 0644);
#else
  if (fd >= 0 && have_mode) {
    (void)!fchown(fd, mst.st_uid, mst.st_gid); /* first: a change of owner clears the set-id bits that the chmod below then restores if the original had them */
    (void)fchmod(fd, mst.st_mode & 07777);
  }
#endif
#if !defined(FK_BUG_NO_FILE_LOCK) && !defined(FK_BUG_COMPACT_DROPS_LOCK)
  /* Lock the replacement BEFORE it is renamed over the store. The lock belongs to the open file and so to its inode: if the new file were locked only after the rename, or never,
   * there would be a moment, or for good, when the store is a file nobody holds a lock on, and a second opener would be let in while this one still writes (found by an independent
   * review of 6f9e979: two writers, B's acknowledged commit lost). EWOULDBLOCK is a proven conflict; any other failure to lock is not, as in fk_file_open. */
  if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) { close(fd); unlink(tp); free(tp); return -1; }
#endif
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

#ifdef FK_TEST_HOOKS
/* For tests only (never defined in a build that is shipped): called once, between the open() and the flock() of the next fk_file_open, then cleared. test/file_test.c uses it to run a
 * compaction at the one instant at which an opener's file can be replaced under it, which makes the race deterministic. */
void (*fk_test_after_open_hook)(void);
#endif

int fk_file_open_ex(const char *path, fk_io *io, int flags) {
  int existed = access(path, F_OK) == 0;
  int fd = -1;
  char *p = NULL;
  /* Open, lock, then check that what was locked is still the file at the path. A compaction in the process that holds the store renames a new file over it; an opener that had
   * opened the old file just before that, and locked it just after the holder closed it, would hold a lock on a file nobody can reach any more and write into it. So the opener
   * compares the file it locked with the file the path names now, and starts again if they differ (the new file is locked by the holder: it gets FK_ELOCKED).
   * The path is also resolved here, once, to an absolute path without symbolic links, and that is the path every later operation uses (the temporary file, the rename over the
   * store, the directory sync). A relative path would follow a chdir() made after the open and send a compaction to another directory; a symbolic link would be replaced by a
   * regular file and the file it points to left behind. */
  for (int attempt = 0; attempt < 16; attempt++) {
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
#ifdef FK_TEST_HOOKS
    if (fk_test_after_open_hook) { void (*h)(void) = fk_test_after_open_hook; fk_test_after_open_hook = NULL; h(); }
#endif
#ifndef FK_BUG_NO_FILE_LOCK
    /* See fk_file_open in fkstore.h. EWOULDBLOCK is a proven conflict; any other failure to lock (ENOLCK, EOPNOTSUPP) is not. */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) { close(fd); return FK_ELOCKED; }
#endif
#ifdef FK_BUG_NO_REALPATH
    p = strdup(path);
#else
    p = realpath(path, NULL);
#endif
    if (!p && errno != ENOENT) { close(fd); return -1; } /* ENOENT: the file went away since the open; look again */
#ifdef FK_BUG_NO_INODE_RECHECK
    if (p) break;
#else
    struct stat have, now;
    if (p && fstat(fd, &have) == 0 && stat(p, &now) == 0 && have.st_dev == now.st_dev && have.st_ino == now.st_ino) break;
#endif
    close(fd);
    fd = -1;
    free(p);
    p = NULL;
  }
  if (fd < 0) return -1; /* the file was replaced under us every time: something is compacting it in a loop */
  FC *c = malloc(sizeof *c);
  if (!c) { free(p); close(fd); return -1; }
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
