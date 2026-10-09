/* fkstore: durable, branchable page store for forkdb. One append-only log file; see docs/DURABLE-STORE.md.
 * Single process, one writer transaction at a time. Not thread-safe. */
#ifndef FKSTORE_H
#define FKSTORE_H

#include <stddef.h>
#include <stdint.h>

#define FK_PAGE 4096u
#define FK_MAX_BRANCHES 1024u

enum {
  FK_OK = 0,
  FK_EIO = -1,      /* the I/O layer failed; the store is now fail-stop */
  FK_ECORRUPT = -2, /* data that should be valid is not (read-time checksum, bad header) */
  FK_EINVAL = -3,
  FK_ENOENT = -4,   /* no such branch, or page beyond the branch size */
  FK_EFAILED = -5,  /* an earlier I/O error: close and reopen (recovery) before using the store again */
  FK_ENOMEM = -6,
  FK_EBUSY = -7     /* a transaction is open */
};

/* The only way the store touches storage. Offsets are absolute file offsets. */
typedef struct fk_io {
  void *ctx;
  int64_t (*size)(void *ctx);                                  /* file size, or -1 */
  int (*pread)(void *ctx, uint64_t off, void *buf, size_t n);  /* bytes read (short at EOF), or -1 */
  int (*pwrite)(void *ctx, uint64_t off, const void *buf, size_t n); /* 0 on success, -1 on error */
  int (*truncate)(void *ctx, uint64_t size);                   /* 0 / -1 */
  int (*fsync)(void *ctx);                                     /* 0 / -1; data is durable only after this returns 0 */
} fk_io;

typedef struct fk_store fk_store;

/* Open (and recover) a store, creating it when the file is empty. Recovery truncates a torn tail. */
int fk_open(const fk_io *io, fk_store **out);
void fk_close(fk_store *s);

/* Branches. Branch 0 always exists. Ids are never reused. */
uint32_t fk_branch_count(const fk_store *s);          /* ids handed out so far, dead ones included */
int fk_branch_alive(const fk_store *s, uint32_t b);
int fk_size(const fk_store *s, uint32_t b, uint32_t *pages);
/* Page content (FK_PAGE bytes). Pages inside the size that were never written read as zeros. */
int fk_read(fk_store *s, uint32_t b, uint32_t pgno, void *buf);

/* O(1) in the size of the data: one small record and one fsync. */
int fk_fork(fk_store *s, uint32_t src, uint32_t *dst);
int fk_discard(fk_store *s, uint32_t b);                 /* branch 0 cannot be discarded */

/* A transaction: pages written here become visible, all together, when commit returns FK_OK. */
int fk_txn_begin(fk_store *s, uint32_t b);
int fk_txn_write(fk_store *s, uint32_t pgno, const void *page);
int fk_txn_commit(fk_store *s, uint32_t new_size_pages); /* every written pgno must be < new_size_pages */
void fk_txn_abort(fk_store *s);

/* CRC-32C, exposed for tests. crc is the finalised value of the previous call (0 to start). */
uint32_t fk_crc32c(uint32_t crc, const void *p, size_t n);

/* Real-file backend (io_file.c). */
int fk_file_open(const char *path, fk_io *io);
void fk_file_close(fk_io *io);

#endif
