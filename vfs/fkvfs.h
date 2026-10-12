/* fkvfs: a SQLite VFS whose database files are forkdb branches. SPIKE: see docs/VFS-SPIKE.md for what it does and does not do.
 *
 * A database is opened by the name "b<N>", N being a branch id of the store: sqlite3_open_v2("b3", &db, flags, "forkdb").
 * Only the main database file lives in the store. Journals and temporary files are in memory and die with the process.
 * Single process, not thread-safe (like fkstore). WAL is refused. The database page size must be a multiple of FK_PAGE (4096). */
#ifndef FKVFS_H
#define FKVFS_H

#include <stddef.h>
#include <stdint.h>

#include "sqlite3.h"
#include "../store/fkstore.h"

typedef struct FkVfs FkVfs;

typedef struct {
  uint64_t atomic_begin;          /* BEGIN_ATOMIC_WRITE calls */
  uint64_t atomic_commit;         /* COMMIT_ATOMIC_WRITE calls that succeeded */
  uint64_t atomic_rollback;       /* ROLLBACK_ATOMIC_WRITE calls */
  uint64_t atomic_refused;        /* BEGIN refused because pages were already staged */
  uint64_t commit_failed;         /* atomic commits that failed (including injected ones) */
  uint64_t sync_publish;          /* staged pages published at xSync (the journal path) */
  uint64_t unlock_publish;        /* staged pages published when the write lock was dropped (synchronous=OFF) */
  uint64_t unlock_publish_errors; /* ... and how many of those failed (SQLite cannot be told: unlock has no error path) */
  uint64_t sync_publish_errors;   /* publishes at xSync that failed (SQLite is told, and a rollback may swallow it, so a test must look here) */
  uint64_t store_commits;         /* fk_txn_commit calls that succeeded */
  uint64_t pages_published;       /* pages handed to the store */
  uint64_t pages_deduped;         /* staged pages dropped at publish because they equal the committed page */
  uint64_t journal_opens;         /* rollback journals opened */
  uint64_t discarded;             /* uncommitted staged state dropped at close */
} FkVfsStats;

/* Register a VFS called name over store s. The store must outlive the VFS and every connection opened through it. */
int fkvfs_register(fk_store *s, const char *name, FkVfs **out);
/* Unregister and free. All connections must be closed first. */
int fkvfs_unregister(FkVfs *v);
/* After the store was reopened (an I/O error is fail-stop in fkstore), point the VFS at the new handle. All connections must be closed. */
void fkvfs_set_store(FkVfs *v, fk_store *s);

void fkvfs_stats(const FkVfs *v, FkVfsStats *out);
/* Advertise SQLITE_IOCAP_BATCH_ATOMIC (default on). It only does anything in a SQLite built with SQLITE_ENABLE_BATCH_ATOMIC_WRITE. */
void fkvfs_set_batch_atomic(FkVfs *v, int on);
/* Test hook: the next n atomic commits fail. */
void fkvfs_fail_commits(FkVfs *v, int n);
/* Test hook: the next publish fails (a non-fatal store error, as out of memory would be) when it hands over its nth page; 0 turns it off. */
void fkvfs_fail_publish_page(FkVfs *v, int n);
/* The database name for a branch, e.g. "b3". */
void fkvfs_branch_name(uint32_t branch, char *buf, size_t n);

#endif
