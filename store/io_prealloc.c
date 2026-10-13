/* io_prealloc.c: a layer over any fk_io that keeps the log's blocks already written ahead of the end of the log.
 *
 * Why: appending to a file extends its size and allocates blocks, and the sync after it must then commit that metadata through the
 * filesystem journal as well as flush the data. Overwriting blocks that are already allocated and written needs only the data
 * flush. Measured on NVMe and gp3 (docs/VFS-SPIKE.md): a commit that appends costs about three times one that overwrites.
 *
 * How: when a write would pass the end of what has been written so far, the layer first writes zeros up to the next multiple of
 * chunk bytes and syncs, once per chunk; the write then lands in blocks that exist. Nothing about the format changes. The zeros
 * past the end of the log are an invalid record, so recovery treats them as a torn tail and cuts them off (it already does that
 * after a power cut); fk_prealloc_trim cuts them off on a clean close, so a file at rest is exactly the log.
 *
 * State: hi is the end of what the store has written; alloc is how far the file has been written (hi <= alloc). The bytes in
 * [hi, alloc) are zeros. Compaction output goes to the sibling file as it is (it is written once, in bulk); after the rename the
 * state is reset from the new file's size.
 *
 * The header is never preallocated: creating a store is truncate(0), write the header, sync, and recovery takes a file whose header
 * never landed for a creation that was cut short only when the file is exactly header-sized. A power cut after the zero fill and
 * before the header would leave a larger file of zeros, which recovery would (rightly) refuse as damaged. So the first extension
 * is triggered by the first record, after the header is durable. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fkstore.h"

#define ZBLOCK (1u << 20)

typedef struct {
  fk_io in;
  uint64_t chunk, hi, alloc;
  int off; /* an extension failed (disk full?): write plainly until the file is truncated or replaced */
  uint8_t *zero;
} PA;

static int pa_extend(PA *p, uint64_t end) {
  uint64_t target = (end + p->chunk - 1) / p->chunk * p->chunk;
#ifdef FK_BUG_PA_FILL_FROM_ZERO
  uint64_t at = 0; /* mutant: the zero fill starts at the beginning of the file and wipes the log */
#else
  uint64_t at = p->alloc;
#endif
  while (at < target) {
    uint64_t n = target - at < ZBLOCK ? target - at : ZBLOCK;
    if (p->in.pwrite(p->in.ctx, at, p->zero, (size_t)n) != 0) return -1;
    at += n;
  }
  return p->in.fsync(p->in.ctx); /* the blocks and the size are durable now: later commits only flush data */
}

static int64_t pa_size64(void *ctx) { return ((PA *)ctx)->in.size(((PA *)ctx)->in.ctx); }
static int pa_pread(void *ctx, uint64_t off, void *buf, size_t n) { PA *p = ctx; return p->in.pread(p->in.ctx, off, buf, n); }
static int pa_fsync(void *ctx) { PA *p = ctx; return p->in.fsync(p->in.ctx); }

static int pa_pwrite(void *ctx, uint64_t off, const void *buf, size_t n) {
  PA *p = ctx;
  uint64_t end = off + n;
#ifdef FK_BUG_PA_EXTEND_HEADER
  if (!p->off && end > p->alloc) { /* mutant: the header write is preallocated too */
#else
  if (!p->off && end > FK_HEADER_SZ && end > p->alloc) {
#endif
#ifdef FK_BUG_PA_FILL_AFTER_WRITE
    int rc = p->in.pwrite(p->in.ctx, off, buf, n); /* mutant: the data goes in first and the zero fill then overwrites it */
    if (rc != 0) return rc;
    (void)pa_extend(p, end);
    if (end > p->hi) p->hi = end;
    if (end > p->alloc) p->alloc = end;
    return 0;
#else
    if (pa_extend(p, end) == 0) {
      p->alloc = (end + p->chunk - 1) / p->chunk * p->chunk;
    } else {
#ifdef FK_BUG_PA_NO_FALLBACK
      return -1; /* mutant: a failed extension fails the write */
#else
      /* Out of space for the zeros (or the disk refused them): the data itself may still fit. Cut back what the fill wrote, stop
       * extending, and write plainly. The log is exactly as correct as without this layer. */
      (void)p->in.truncate(p->in.ctx, p->alloc);
      p->off = 1;
#endif
    }
#endif
  }
  int rc = p->in.pwrite(p->in.ctx, off, buf, n);
  if (rc != 0) return rc;
  if (end > p->hi) p->hi = end;
  if (end > p->alloc) p->alloc = end;
  return 0;
}

static int pa_truncate(void *ctx, uint64_t size) {
  PA *p = ctx;
  int rc = p->in.truncate(p->in.ctx, size);
  if (rc != 0) return rc;
  p->hi = p->alloc = size; /* the file is exactly this long now */
  p->off = 0;
  return 0;
}

static void pa_reset(PA *p) {
  int64_t sz = p->in.size(p->in.ctx);
  p->hi = p->alloc = sz < 0 ? 0 : (uint64_t)sz;
  p->off = 0;
}

static int pa_sibling_create(void *ctx, fk_io *tmp) { PA *p = ctx; return p->in.sibling_create(p->in.ctx, tmp); }
static int pa_sibling_rename(void *ctx, fk_io *tmp) {
  PA *p = ctx;
  int rc = p->in.sibling_rename(p->in.ctx, tmp);
  if (rc != 0) return rc;
#ifdef FK_BUG_PA_RENAME_FORGETS_STATE
  p->hi = p->alloc = 0; /* mutant: the state is forgotten, so the next extension zero-fills over the compacted log */
#else
  pa_reset(p); /* the file is the compacted one now: nothing is preallocated */
#endif
  return 0;
}
static int pa_sync_dir(void *ctx) { PA *p = ctx; return p->in.sync_dir(p->in.ctx); }
static void pa_sibling_remove(void *ctx, fk_io *tmp) { PA *p = ctx; p->in.sibling_remove(p->in.ctx, tmp); }

int fk_prealloc_wrap(fk_io *io, uint64_t chunk) {
  if (chunk == 0) return FK_OK; /* disabled */
  PA *p = calloc(1, sizeof *p);
  uint8_t *z = calloc(1, ZBLOCK);
  if (!p || !z) { free(p); free(z); return FK_ENOMEM; }
  p->in = *io;
  p->chunk = chunk;
  p->zero = z;
  pa_reset(p);
  io->ctx = p;
  io->size = pa_size64;
  io->pread = pa_pread;
  io->pwrite = pa_pwrite;
  io->truncate = pa_truncate;
  io->fsync = pa_fsync;
  io->sibling_create = p->in.sibling_create ? pa_sibling_create : NULL;
  io->sibling_rename = p->in.sibling_rename ? pa_sibling_rename : NULL;
  io->sync_dir = p->in.sync_dir ? pa_sync_dir : NULL;
  io->sibling_remove = p->in.sibling_remove ? pa_sibling_remove : NULL;
  return FK_OK;
}

static int is_wrapped(const fk_io *io) { return io->ctx && io->pwrite == pa_pwrite; }

/* Zero-fill one chunk ahead of the current log end, when no transaction is open.
 * Call between commits to keep preallocated blocks ahead of the writer, so commits
 * write data into blocks that already exist without pausing to extend the file.
 * Returns 0 on success, -1 if an extension failed (disk full or refused).
 * Idempotent and safe to call on unwrapped I/O. */
int fk_prealloc_background(fk_io *io) {
  if (!is_wrapped(io)) return 0;
  PA *p = io->ctx;
  if (p->off) return -1;
  uint64_t end = p->hi + p->chunk;
  if (end <= p->alloc) return 0;
  if (pa_extend(p, end) != 0) { (void)p->in.truncate(p->in.ctx, p->alloc); p->off = 1; return -1; }
  p->alloc = (end + p->chunk - 1) / p->chunk * p->chunk;
  return 0;
}

void fk_prealloc_trim(const fk_io *io) {
  if (!is_wrapped(io)) return;
  PA *p = io->ctx;
  int64_t sz = p->in.size(p->in.ctx);
#ifdef FK_BUG_PA_TRIM_SHORT
  if (sz > 0 && (uint64_t)sz > p->hi - 1) { (void)p->in.truncate(p->in.ctx, p->hi - 1); p->alloc = p->hi - 1; }
  return;
#endif
  if (sz >= 0 && (uint64_t)sz > p->hi) {
    if (p->in.truncate(p->in.ctx, p->hi) == 0) p->alloc = p->hi;
  }
}

void fk_prealloc_release(fk_io *io) {
  if (!is_wrapped(io)) return;
  PA *p = io->ctx;
  fk_io inner = p->in;
  free(p->zero);
  free(p);
  *io = inner;
}

void fk_prealloc_close(fk_io *io) {
  fk_prealloc_trim(io);
  fk_prealloc_release(io);
}
