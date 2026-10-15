/* io_file_win.c: fk_io on a real file using the Win32 API (CreateFileW / ReadFile / WriteFile).
 *
 * Why: Windows has no pread/pwrite, no fdatasync, no rename-over-open semantics.
 * This backend emulates them on top of the Win32 API.
 *
 * Limitations:
 * - pread/pwrite are emulated with SetFilePointerEx → Read/WriteFile → restore offset.
 *   This is not atomic if the handle is shared, but forkdb is single-writer per branch.
 * - Compaction sibling rename uses MoveFileExW with MOVEFILE_REPLACE_EXISTING, which is
 *   close to POSIX rename() but not identical. The old fd is closed and reopened from the
 *   new path after the rename completes.
 * - File paths use UTF-8 → wide-char conversion via MultiByteToWideChar. Paths longer
 *   than MAX_PATH (260 chars) need the \\?\ prefix.
 */
#define _GNU_SOURCE
#include "fkstore.h"

#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  HANDLE h;
  wchar_t *path;
  int nosync; /* FK_FILE_NOSYNC: the flushes are not issued */
} WC;

#define H(ctx) (((WC *)(ctx))->h)

/* ---------- wide-char path helpers ---------- */

static wchar_t *to_wpath(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  /* use \\?\ prefix for long path support */
  int prefixed = 0;
  size_t slen = strlen(s);
  if (slen >= 2 && s[1] == ':') prefixed = 1;
  wchar_t *w = malloc((size_t)(n + (prefixed ? 4 : 0)) * sizeof(wchar_t));
  if (!w) return NULL;
  if (prefixed) {
    w[0] = L'\\'; w[1] = L'\\'; w[2] = L'?'; w[3] = L'\\';
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w + 4, n);
  } else {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  }
  return w;
}

static wchar_t *tmp_wpath(const wchar_t *p) {
  size_t n = wcslen(p);
  wchar_t *t = malloc((n + 9) * sizeof(wchar_t));
  if (!t) return NULL;
  memcpy(t, p, n * sizeof(wchar_t));
  wcscpy(t + n, L".compact");
  return t;
}

/* ---------- I/O operations ---------- */

static int64_t w_size(void *ctx) {
  LARGE_INTEGER li;
  if (!GetFileSizeEx(H(ctx), &li)) return -1;
  return li.QuadPart;
}

static int w_pread(void *ctx, uint64_t off, void *buf, size_t n) {
  HANDLE h = H(ctx);
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)off;

  /* save current position */
  LARGE_INTEGER oldpos = {0};
  SetFilePointerEx(h, (LARGE_INTEGER){0}, &oldpos, FILE_CURRENT);

  /* seek to offset, read, restore */
  if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return -1;

  size_t done = 0;
  while (done < n) {
    DWORD nr = 0;
    if (!ReadFile(h, (char *)buf + done, (DWORD)(n - done > MAXDWORD ? MAXDWORD : n - done), &nr, NULL)) {
      if (GetLastError() == ERROR_OPERATION_ABORTED) continue;
      SetFilePointerEx(h, oldpos, NULL, FILE_BEGIN);
      return -1;
    }
    if (nr == 0) break;
    done += nr;
  }

  SetFilePointerEx(h, oldpos, NULL, FILE_BEGIN);
  return (int)done;
}

static int w_pwrite(void *ctx, uint64_t off, const void *buf, size_t n) {
  HANDLE h = H(ctx);
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)off;

  LARGE_INTEGER oldpos = {0};
  SetFilePointerEx(h, (LARGE_INTEGER){0}, &oldpos, FILE_CURRENT);

  if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return -1;

  size_t done = 0;
  while (done < n) {
    DWORD nw = 0;
    if (!WriteFile(h, (const char *)buf + done, (DWORD)(n - done > MAXDWORD ? MAXDWORD : n - done), &nw, NULL)) {
      if (GetLastError() == ERROR_OPERATION_ABORTED) continue;
      SetFilePointerEx(h, oldpos, NULL, FILE_BEGIN);
      return -1;
    }
    done += nw;
  }

  SetFilePointerEx(h, oldpos, NULL, FILE_BEGIN);
  return 0;
}

static int w_truncate(void *ctx, uint64_t size) {
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)size;
  if (!SetFilePointerEx(H(ctx), li, NULL, FILE_BEGIN)) return -1;
  if (!SetEndOfFile(H(ctx))) return -1;
  return 0;
}

static int w_fsync(void *ctx) {
  if (((WC *)ctx)->nosync) return 0;
  return FlushFileBuffers(H(ctx)) ? 0 : -1;
}

/* Forward declarations (called from fill() before they are defined) */
static int w_sibling_create(void *ctx, fk_io *tmp);
static int w_sibling_rename(void *ctx, fk_io *tmp);
static int w_sync_dir(void *ctx);
static void w_sibling_remove(void *ctx, fk_io *tmp);

static int sync_dir_of(const wchar_t *path) {
  /* Get the parent directory path */
  size_t n = wcslen(path);
  wchar_t *dir = malloc((n + 1) * sizeof(wchar_t));
  if (!dir) return -1;
  memcpy(dir, path, (n + 1) * sizeof(wchar_t));
  while (n > 0 && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
  if (n > 1) dir[n - 1] = 0; else { dir[0] = L'.'; dir[1] = 0; }

  HANDLE h = CreateFileW(dir, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(dir);
  if (h == INVALID_HANDLE_VALUE) return -1;
  int rc = FlushFileBuffers(h) ? 0 : -1;
  CloseHandle(h);
  return rc;
}

static void fill(fk_io *io, WC *c, int main_file) {
  memset(io, 0, sizeof *io);
  io->ctx = c;
  io->size = w_size;
  io->pread = w_pread;
  io->pwrite = w_pwrite;
  io->truncate = w_truncate;
  io->fsync = w_fsync;
  if (main_file) {
    io->sibling_create = w_sibling_create;
    io->sibling_rename = w_sibling_rename;
    io->sync_dir = w_sync_dir;
    io->sibling_remove = w_sibling_remove;
  }
}

/* ---------- sibling (compaction) support ---------- */

static int w_sibling_create(void *ctx, fk_io *tmp) {
  WC *m = ctx;
  wchar_t *tp = tmp_wpath(m->path);
  if (!tp) return -1;

  HANDLE h = CreateFileW(tp, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) { free(tp); return -1; }

  WC *t = malloc(sizeof *t);
  if (!t) { CloseHandle(h); DeleteFileW(tp); free(tp); return -1; }
  t->h = h;
  t->path = tp;
  t->nosync = m->nosync;
  fill(tmp, t, 0);
  return 0;
}

static int w_sibling_rename(void *ctx, fk_io *tmp) {
  WC *m = ctx, *t = tmp->ctx;
  /* On Windows, you cannot rename a file that is open without FILE_SHARE_DELETE.
   * Close the temp file before renaming. */
  CloseHandle(t->h);
  t->h = NULL;

  if (!MoveFileExW(t->path, m->path, MOVEFILE_REPLACE_EXISTING))
    return -1;

  /* Close the old file handle and reopen using the new path.
   * (The old handle still refers to the now-replaced file.)
   * Share mode must match fk_file_open: FILE_SHARE_READ keeps concurrent readers working. */
  CloseHandle(m->h);
  m->h = CreateFileW(m->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (m->h == INVALID_HANDLE_VALUE) return -1;

  free(t->path);
  free(t);
  tmp->ctx = NULL;
  return 0;
}

static int w_sync_dir(void *ctx) {
  if (((WC *)ctx)->nosync) return 0;
  return sync_dir_of(((WC *)ctx)->path);
}

static void w_sibling_remove(void *ctx, fk_io *tmp) {
  WC *m = ctx;
  if (tmp && tmp->ctx) {
    WC *t = tmp->ctx;
    /* Rename closes this handle first. A failed MoveFileExW leaves it NULL, and
     * CloseHandle(NULL) raises STATUS_INVALID_HANDLE under a debugger. */
    if (t->h != NULL && t->h != INVALID_HANDLE_VALUE)
      CloseHandle(t->h);
    DeleteFileW(t->path);
    free(t->path);
    free(t);
    tmp->ctx = NULL;
    return;
  }
  wchar_t *tp = tmp_wpath(m->path);
  if (tp) { DeleteFileW(tp); free(tp); }
}

/* ---------- open / close ---------- */

int fk_file_open(const char *path, fk_io *io) { return fk_file_open_ex(path, io, 0); }

int fk_file_open_ex(const char *path, fk_io *io, int flags) {
  wchar_t *wp = to_wpath(path);
  if (!wp) return -1;

  DWORD creation = (GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES) ? OPEN_EXISTING : CREATE_NEW;
  HANDLE h = CreateFileW(wp, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ, NULL,
                         creation, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    /* The share mode (FILE_SHARE_READ only) is the lock: a second opener that asks for write access is refused by the system. */
    int busy = GetLastError() == ERROR_SHARING_VIOLATION;
    free(wp);
    return busy ? FK_ELOCKED : -1;
  }

  WC *c = malloc(sizeof *c);
  if (!c) { CloseHandle(h); free(wp); return -1; }
  c->h = h;
  c->path = wp;
  c->nosync = (flags & FK_FILE_NOSYNC) != 0;
  fill(io, c, 1);
  return 0;
}

void fk_file_close(fk_io *io) {
  if (io->ctx) {
    WC *c = io->ctx;
    CloseHandle(c->h);
    free(c->path);
    free(c);
    io->ctx = NULL;
  }
}

#endif /* _WIN32 */
