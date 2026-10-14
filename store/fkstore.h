/* fkstore: durable, branchable page store for forkdb. One append-only log file; see docs/DURABLE-STORE.md.
 * One process (the file is locked, see fk_file_open), one writer transaction at a time. Not thread-safe. */
#ifndef FKSTORE_H
#define FKSTORE_H

#include <stddef.h>
#include <stdint.h>

#define FK_PAGE 4096u
#ifndef FK_MAX_BRANCHES
#define FK_MAX_BRANCHES 1024u /* branch ids are below this: at most this many branches alive at once (tests build with a smaller value) */
#endif
#define FK_META_MAX 512u /* the most bytes of metadata a branch can carry */
#define FK_MAX_PAGES (1u << 28) /* page numbers and sizes are below this (1 TiB at 4 KiB pages) */

enum {
  FK_OK = 0,
  FK_EIO = -1,      /* the I/O layer failed; the store is now fail-stop */
  FK_ECORRUPT = -2, /* data that should be valid is not (read-time checksum, bad header) */
  FK_EINVAL = -3,
  FK_ENOENT = -4,   /* no such branch, or page beyond the branch size */
  FK_EFAILED = -5,  /* an earlier I/O error: close and reopen (recovery) before using the store again */
  FK_ENOMEM = -6,
  FK_EBUSY = -7,    /* a transaction is open */
  FK_ELIMIT = -8,   /* fork: FK_MAX_BRANCHES branches are alive; discard one first */
  FK_EVERSION = -9, /* the file is older than format version 4: compact it (fk_compact rewrites it in the current format) to use this */
  FK_ELOCKED = -10  /* fk_file_open: the file is already open, in another process or in this one (see fk_file_open) */
};

/* The only way the store touches storage. Offsets are absolute file offsets. */
typedef struct fk_io {
  void *ctx;
  int64_t (*size)(void *ctx);                                  /* file size, or -1 */
  int (*pread)(void *ctx, uint64_t off, void *buf, size_t n);  /* bytes read (short at EOF), or -1 */
  int (*pwrite)(void *ctx, uint64_t off, const void *buf, size_t n); /* 0 on success, -1 on error */
  int (*truncate)(void *ctx, uint64_t size);                   /* 0 / -1 */
  int (*fsync)(void *ctx);                                     /* 0 / -1; data is durable only after this returns 0 */
  /* Compaction support (optional; without these fk_compact returns FK_EINVAL). A "sibling" is a second, temporary file next
   * to the store's file, with the same operations, that can atomically replace it. */
  int (*sibling_create)(void *ctx, struct fk_io *tmp);         /* create or truncate the temporary file; fill *tmp (size/pread/pwrite/truncate/fsync) */
  int (*sibling_rename)(void *ctx, struct fk_io *tmp);         /* rename it over the store's file; afterwards ctx is the new file and tmp is consumed. On
                                                                  failure nothing has changed. The rename is NOT durable until sync_dir returns 0. */
  int (*sync_dir)(void *ctx);                                  /* make the directory entry durable: 0 / -1 */
  void (*sibling_remove)(void *ctx, struct fk_io *tmp);        /* close and delete the temporary file (tmp may be NULL: delete a leftover from an earlier run) */
} fk_io;

#define FK_HEADER_SZ 4096u /* the file header: the first FK_HEADER_SZ bytes; records start after it */

/* Preallocation (store/io_prealloc.c). A layer over any fk_io that writes zeros ahead of the end of the log, a chunk at a time, so
 * that a commit overwrites blocks that already exist instead of growing the file: a commit that appends costs about three times as
 * much on real disks (docs/VFS-SPIKE.md). The format does not change: the zeros past the end of the log are a torn tail, which
 * recovery already cuts off. Opt-in.
 *
 *   fk_io io; fk_file_open(path, &io);
 *   fk_prealloc_wrap(&io, FK_PREALLOC_DEFAULT);   // chunk 0 leaves io as it is
 *   fk_store *s; fk_open(&io, &s); ...; fk_close(s);
 *   fk_prealloc_close(&io);                       // cut the file back to the log end and free the layer (io is the file again)
 *   fk_file_close(&io);
 *
 * Without the trim (a crash) the file ends in zeros, which the next open cuts off. If the zeros cannot be written (the disk is
 * full) the layer stops extending and writes plainly, so it never fails a write the file itself would have accepted. The compaction
 * output is not preallocated (it is written once, in bulk). The cost: every byte of log is written twice, once as zeros. */
#define FK_PREALLOC_DEFAULT (4u << 20)
int fk_prealloc_wrap(fk_io *io, uint64_t chunk);
void fk_prealloc_trim(const fk_io *io);     /* cut the file back to the end of what was written (no-op if io is not wrapped) */
void fk_prealloc_release(fk_io *io);        /* free the layer, no trim; io is the underlying io again */
void fk_prealloc_close(fk_io *io);          /* trim, then release */
/* Call between commits: zero-fill one chunk ahead so the next commit writes into
 * blocks that exist without pausing to extend the file. Returns 0 on success,
 * -1 if an extension failed. Safe on unwrapped I/O (returns 0). */
int fk_prealloc_background(fk_io *io);

typedef struct fk_store fk_store;

/* Open (and recover) a store, creating it when the file is empty. Recovery truncates a torn tail. */
int fk_open(const fk_io *io, fk_store **out);
void fk_close(fk_store *s);

/* Branches. Branch 0 always exists. A fork is given an id below FK_MAX_BRANCHES: a never-used one, or the id of a discarded branch,
 * searched next-fit (starting just after the id the latest fork got and wrapping round), so a discarded id comes back as late as
 * possible. The caller must drop its handle when it discards a branch: a later fork may be given the same number, and a stale
 * handle would then read the new branch. (Files written before format version 3 never reuse ids, until they are compacted.) */
uint32_t fk_branch_count(const fk_store *s);          /* one more than the highest id ever in use; dead ids below it are not alive */
int fk_branch_alive(const fk_store *s, uint32_t b);
int fk_size(const fk_store *s, uint32_t b, uint32_t *pages);
/* Page content (FK_PAGE bytes). Pages inside the size that were never written read as zeros. */
int fk_read(fk_store *s, uint32_t b, uint32_t pgno, void *buf);

/* O(1) in the size of the data: one small record and one fsync. */
int fk_fork(fk_store *s, uint32_t src, uint32_t *dst);
int fk_discard(fk_store *s, uint32_t b);                 /* branch 0 cannot be discarded */

/* Format version 4: state ids, metadata, reset. Files written by this version are version 4; an older file keeps opening and working
 * as before, and the calls below that write return FK_EVERSION on it until it is compacted (fk_compact writes the current format).
 * fk_version says which the open file is.
 *
 * State id: a number that says which content a branch holds. A commit gives the branch a new one that has never been used for any
 * other content; fork and reset give the new branch the id of its source. So two live branches hold the same content when their ids
 * are equal (and, as ids are never reused for different content, only then). Ids are kept across close, recovery and compaction, but
 * they mean nothing across stores or after a file written by an older version is first compacted. Reads never fail on a v3 file. */
int fk_version(const fk_store *s);
int fk_state(const fk_store *s, uint32_t b, uint64_t *state);
/* Metadata: up to FK_META_MAX bytes the layer above keeps with a branch (a name, what it is for). Replaced whole (length 0 clears it),
 * one record and one fsync, kept across recovery and compaction, dropped when the branch is discarded (so a branch that later gets the
 * same id starts without). A plain fork starts without metadata. fk_meta_get copies at most cap bytes and always sets *len to the full length. */
int fk_meta_set(fk_store *s, uint32_t b, const void *meta, size_t len);
int fk_meta_get(const fk_store *s, uint32_t b, void *buf, size_t cap, size_t *len);
/* fork and give the new branch its metadata in the same record: there is no state in which the branch exists without it. */
int fk_fork_meta(fk_store *s, uint32_t src, const void *meta, size_t len, uint32_t *dst);
/* dst takes the content (and size, and state id) of src, which stays as it is; dst's metadata is kept. O(1): one small record and one
 * fsync. The old content of dst is no longer reachable (compaction frees it). dst may be branch 0. Needs no open transaction.
 * The caller must make sure nothing still holds the old content of dst in a cache (the VFS refuses while a connection is open on it). */
int fk_reset(fk_store *s, uint32_t dst, uint32_t src);

/* A transaction: pages written here become visible, all together, when commit returns FK_OK. */
int fk_txn_begin(fk_store *s, uint32_t b);
int fk_txn_write(fk_store *s, uint32_t pgno, const void *page);
/* The same, without the copy: page must be a malloc'd buffer of FK_PAGE bytes. On FK_OK the store owns it (it frees it at commit or
 * abort, so the caller must not touch it again); on any error the caller still owns it. For a caller that stages a transaction's
 * pages anyway and would otherwise hold every page twice until the commit. */
int fk_txn_write_owned(fk_store *s, uint32_t pgno, void *page);
int fk_txn_commit(fk_store *s, uint32_t new_size_pages); /* every written pgno must be < new_size_pages */
void fk_txn_abort(fk_store *s);

/* Compaction: rewrite the log so it holds only what the live branches can still see (see docs/DURABLE-STORE.md).
 * Blocks the caller for its duration. Needs the sibling operations in fk_io, and no open transaction.
 * An error before the replacement leaves the store untouched and usable; an error after it is fail-stop (reopen). */
int fk_compact(fk_store *s);
/* Compacts only when the file is at least min_bytes and at least half of it is garbage. *did is 1 if it compacted. */
int fk_compact_if_needed(fk_store *s, uint64_t min_bytes, int *did);
/* The file size, and the size a compacted file would have. */
int fk_stats(fk_store *s, uint64_t *file_bytes, uint64_t *live_bytes);
int fk_is_failed(const fk_store *s); /* after an I/O error that needs a reopen */
/* Memory used by the in-memory index (layers and their page tables), in bytes. For monitoring and tests. */
uint64_t fk_index_bytes(const fk_store *s);
/* The layer tree: how many layers exist, and how many a read on branch b may have to look through (1 = the head only). Layers that
 * no branch needs are freed when a branch is discarded, and a layer that only one other layer builds on is merged into it, so
 * these stay small however many forks and discards have happened. For monitoring and tests. */
uint32_t fk_layer_count(const fk_store *s);
int fk_branch_depth(const fk_store *s, uint32_t b, uint32_t *depth);

/* CRC-32C, exposed for tests. crc is the finalised value of the previous call (0 to start). Uses the CPU's CRC32 instruction when
 * there is one (x86-64: SSE4.2, checked at run time) and a portable loop otherwise; both give the same result. */
uint32_t fk_crc32c(uint32_t crc, const void *p, size_t n);
/* For tests: choose the implementation (0 = portable, 1 = hardware if available). Returns 1 if the hardware one is now in use.
 * Not thread-safe; call it before any store is open. */
int fk_crc32c_select(int want_hw);

/* Real-file backend (io_file.c). fk_file_open returns 0, FK_ELOCKED, or -1 for any other failure (so callers that test != 0 keep working).
 * One opener at a time: it takes an exclusive lock on the file (flock on POSIX; on Windows the share mode already does it), held until
 * fk_file_close or the process ends, so a process that dies releases it. Two processes writing one store file lose acknowledged commits and
 * can leave a file that no longer opens (measured: 8 of 8 runs), so the second one is refused instead. The lock belongs to the open file
 * description: a second fk_file_open of the same path in this process is refused as well, and fork() children share the parent's. A
 * filesystem that cannot lock (ENOLCK, EOPNOTSUPP: some network filesystems) is not refused, only a proven conflict is. */
int fk_file_open(const char *path, fk_io *io);
void fk_file_close(fk_io *io);

#endif
