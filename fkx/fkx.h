/* fkx: the branch layer of forkdb (docs/FKX-DESIGN.md). Implemented in fkx/fkx.c (the core) and fkx/fkx_session.c (diff, replay promote, undo by replay and
 * the stepwise primitives, on SQLite's session extension). A build without the session driver links fkx/fkx_nosession.c instead: fkx_have_session() is 0,
 * fkx_diff, fkx_diff_open and fkx_replay_begin return FKX_ENOSESSION, and fkx_promote and fkx_undo only do what a fast-forward and a reset can (anything
 * that would need a replay returns FKX_ENOSESSION and changes nothing).
 *
 * fkx sits on the durable store (store/fkstore.h) and the SQLite VFS (vfs/fkvfs.h). It gives branches names, keeps what a fork was forked
 * from, and implements fork, snapshot, reset, discard, diff, promote and undo. It takes SQLite as given: it never links a second copy, and
 * the parts that need SQLite's session extension (diff, replay) are a separate, optional driver (fkx_session) behind the same API, with
 * stepwise primitives (fkx_diff_open, fkx_replay_begin) for hosts whose SQLite has the session API but cannot give it to C code.
 *
 * Single process, single thread (like the store and the VFS). A transaction writes one branch (see SESSION-DESIGN.md).
 *
 * Branches are named by a string: a user name ("main", "agent-17"), or "#" and 16 hex digits for the uid of any branch, which is how
 * the internal branches (bases, undo points) are addressed. Store branch ids are reused and are never part of this API except in
 * fkx_info.id, which is only valid until the next call that changes the repository. */
#ifndef FKX_H
#define FKX_H

#include <stddef.h>
#include <stdint.h>

#include "sqlite3.h"
#include "../store/fkstore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fkx fkx;

/* Results. Zero is success; everything else is negative. fkx_errmsg says what was wrong, in words, for the last failing call. */
enum {
  FKX_OK = 0,
  FKX_ENOENT = -1,      /* no such branch */
  FKX_EEXIST = -2,      /* the name is taken */
  FKX_EINVAL = -3,      /* bad argument: a name that is not allowed, an operation that makes no sense for this kind of branch */
  FKX_EBUSY = -4,       /* a connection is open on the branch, or a store transaction is open */
  FKX_EREADONLY = -5,   /* the branch is a snapshot, a base or an undo point */
  FKX_ECONFLICT = -6,   /* a replay hit a conflict; nothing was changed; see the result's conflict list */
  FKX_EREFUSED = -7,    /* promote or undo refused by the schema policy (DDL, a table without a primary key, ...); nothing was changed */
  FKX_ENOSESSION = -8,  /* this build or host has no session API: diff, replay, promote by replay and undo by replay are not available */
  FKX_EVERSION = -9,    /* the store file is older than format version 4: open it with options.upgrade */
  FKX_ESTORE = -10,     /* the store failed: fkx_store_rc has its code. The store is fail-stop: close and reopen */
  FKX_ESQLITE = -11,    /* SQLite failed: fkx_sqlite_rc has its code */
  FKX_ENOMEM = -12,
  FKX_ELIMIT = -13,     /* out of store branch ids (FK_MAX_BRANCHES), or a name or a metadata blob too long */
  FKX_ECORRUPT = -14,   /* the store holds branches fkx did not make, or metadata that does not decode */
  FKX_ELOCKED = -15     /* fkx_open: the store file is open in another process, or already open in this one */
};

#define FKX_NAME_MAX 63 /* bytes; letters, digits and . _ - / ; not empty; not starting with '#' */
#define FKX_UNDO_KEEP_MAX 16

typedef enum {
  FKX_ROOT = 1,      /* branch 0: where everything started; can be promoted into, cannot be discarded */
  FKX_FORK = 2,      /* a named, writable branch with an origin and a base */
  FKX_SNAPSHOT = 3,  /* a named, read-only copy of a branch at one moment */
  FKX_BASE = 4,      /* internal, read-only: the state a fork was forked from; found by uid, never named */
  FKX_UNDO = 5       /* internal, read-only: one half of an undo point */
} fkx_kind;

typedef struct {
  char name[FKX_NAME_MAX + 1]; /* "" for internal kinds */
  fkx_kind kind;
  int read_only;
  uint64_t uid;        /* 64 random bits, never reused: the stable identity of a branch */
  uint64_t origin_uid; /* fork: the branch it was forked from (0 if that no longer exists); snapshot: the branch it was taken from */
  uint64_t base_uid;   /* fork: its base (the fork point); otherwise 0 */
  uint64_t owner_uid;  /* base and undo points: the branch they belong to; otherwise 0 */
  uint64_t state;      /* the store's state id: equal state ids mean equal content (fk_state) */
  uint32_t size_pages; /* database size in 4 KiB pages */
  uint32_t n_undo;     /* undo points kept for this branch */
  uint32_t open_connections; /* connections open on it through the VFS right now */
  uint32_t id;         /* the store's branch id: transient, for diagnostics */
} fkx_info;

typedef struct {
  const char *vfs_name;     /* the SQLite VFS name to register; default "forkdb" */
  const char *root_name;    /* name of branch 0 in a new repository; default "main" */
  uint64_t prealloc_chunk;  /* log preallocation (store/io_prealloc.c); default FK_PREALLOC_DEFAULT, 0 turns it off */
  unsigned undo_keep;       /* undo points kept per branch; default 8, at most FKX_UNDO_KEEP_MAX */
  int create;               /* create the file if it does not exist (default 1) */
  int upgrade;              /* a store older than version 4 is compacted into the current format instead of refused with FKX_EVERSION */
  uint64_t (*random64)(void *ctx); /* source of uids; default: the operating system's (getrandom); a test can inject a sequence */
  void *random_ctx;
} fkx_options;

void fkx_options_default(fkx_options *o);

/* ---- the repository ---- */

/* Open (or create) a repository: the store file, the VFS, the name index. Garbage left by an interrupted operation is collected here
 * (fkx_gc). A store whose branches were not made by fkx is refused with FKX_ECORRUPT. */
int fkx_open(const char *path, const fkx_options *o, fkx **out);
/* All connections must be closed. Trims the preallocated tail, closes the store. */
void fkx_close(fkx *x);
const char *fkx_errmsg(const fkx *x);
int fkx_store_rc(const fkx *x);
int fkx_sqlite_rc(const fkx *x);
const char *fkx_vfs_name(const fkx *x);
/* The store, for read-only queries (fk_stats, fk_size, ...). Writing to it behind fkx's back breaks its invariants. */
const fk_store *fkx_store(const fkx *x);

/* ---- branches ---- */

/* Fork a new writable branch from "from". Makes two store branches (the base, then the fork), so it is not atomic: a cut between them
 * leaves an orphaned base, which fkx_gc removes. O(1) in the size of the data. */
int fkx_fork(fkx *x, const char *from, const char *name);
/* A named read-only copy of "from" now. One store record. */
int fkx_snapshot(fkx *x, const char *from, const char *name);
/* Discard a fork or a snapshot, with its base and undo points. FKX_EBUSY if a connection is open on any of them. The root cannot be discarded. */
int fkx_discard(fkx *x, const char *name);
/* "name" takes the content of "to" (a snapshot, a fork, or the root); to == NULL means its own base, which throws the fork's work away.
 * One store record, O(1). FKX_EBUSY while any connection is open on "name" (see the note on caches in DURABLE-STORE.md). Not undoable. */
int fkx_reset(fkx *x, const char *name, const char *to);
int fkx_info_get(fkx *x, const char *ref, fkx_info *out);
/* Fills at most cap entries and sets *n to the number that exist. Internal branches are listed only if include_internal. */
int fkx_list(fkx *x, int include_internal, fkx_info *out, size_t cap, size_t *n);
/* Open a connection on a branch. flags are sqlite3_open_v2 flags (READWRITE is refused with FKX_EREADONLY on a read-only branch). The
 * connection is an ordinary SQLite connection: close it with sqlite3_close. A write transaction on it writes this branch only. */
int fkx_open_db(fkx *x, const char *ref, int flags, sqlite3 **db);

/* ---- maintenance ---- */

typedef struct { uint32_t orphan_bases, orphan_undo, unlisted_undo, over_retention; } fkx_gc_report;
/* Discard what an interrupted operation or a lowered retention left behind: bases and undo points whose owner is gone, undo points the
 * owner does not list, undo points past undo_keep. Never touches a named branch. Fails with FKX_EBUSY on branches that are open (they stay). */
int fkx_gc(fkx *x, fkx_gc_report *report);
/* fk_compact_if_needed on the store. FKX_EBUSY while a store transaction is open. */
int fkx_compact_if_needed(fkx *x, uint64_t min_bytes, int *did);

/* Open-time checkpoint threshold (from the store). A store opened from a log (not a
 * snapshot) and larger than this is compacted to make the next open fast. */
#define FKX_CHECKPOINT_MIN_BYTES ((uint64_t)16 << 20)

/* ---- diff, promote, undo: the session driver. Everything below returns FKX_ENOSESSION where fkx_have_session() is 0. ---- */

/* 1 if this build has the driver (it was compiled against a SQLite with SQLITE_ENABLE_SESSION and SQLITE_ENABLE_PREUPDATE_HOOK). */
int fkx_have_session(void);

typedef enum {
  FKX_TABLE_COMPARED = 0,   /* diffed */
  FKX_TABLE_NO_PK = 1,      /* no PRIMARY KEY: not diffable; rows_differ says whether its rows differ at all (compared with EXCEPT) */
  FKX_TABLE_ONLY_HERE = 2,  /* not in the other branch */
  FKX_TABLE_ONLY_THERE = 3, /* only in the other branch */
  FKX_TABLE_SCHEMA = 4,     /* in both, definitions differ (PRAGMA table_xinfo) */
  FKX_TABLE_VIRTUAL = 5     /* a virtual table: not compared */
} fkx_table_status;

typedef struct {
  char *name;
  fkx_table_status status;
  uint64_t inserts, updates, deletes; /* from the changeset; 0 unless status is COMPARED */
  int rows_differ;                    /* NO_PK: 1 if the rows differ */
} fkx_table_diff;

typedef struct {
  fkx_table_diff *tables; /* every table of either side, so nothing is left out silently */
  size_t ntables;
  int schema_differs;     /* sqlite_schema differs in anything but table row content (DDL) */
  void *changeset;        /* sqlite3session changeset bytes, for the COMPARED tables; free with fkx_diff_result_free */
  int nchangeset;
} fkx_diff_result;

/* What changed from "against" (default: the branch's base) to "ref". */
int fkx_diff(fkx *x, const char *ref, const char *against, fkx_diff_result *out);
void fkx_diff_result_free(fkx_diff_result *r);

typedef enum { FKX_METHOD_NONE = 0, FKX_METHOD_UP_TO_DATE = 1, FKX_METHOD_FAST_FORWARD = 2, FKX_METHOD_REPLAY = 3 } fkx_method;

typedef struct {
  char *table;
  int kind;       /* sqlite3changeset conflict kind: DATA, NOTFOUND, CONFLICT, CONSTRAINT, FOREIGN_KEY */
  int op;         /* SQLITE_INSERT, SQLITE_UPDATE or SQLITE_DELETE */
  char *key;      /* the primary key, as text */
} fkx_conflict;

typedef struct {
  fkx_method method;
  int dry_run;
  fkx_conflict *conflicts; /* on FKX_ECONFLICT: the first one, or all of them in a dry run */
  size_t nconflicts;
  char *refusal;           /* on FKX_EREFUSED: which rule, which table */
  int undo_recorded;       /* an undo point was recorded for this promote */
} fkx_promote_result;

typedef struct {
  const char *into; /* target; default: the fork's origin */
  int dry_run;      /* run everything, change nothing, list every conflict */
} fkx_promote_opts;

/* Put the work of fork "name" into its target. Fast-forward if the target has not moved since the fork point and no connection is open
 * on it (one store record); otherwise replay in one SQLite transaction on the target (one store commit); see SESSION-DESIGN.md for the
 * schema policy. The target changes in exactly one atomic step or not at all. */
int fkx_promote(fkx *x, const char *name, const fkx_promote_opts *o, fkx_promote_result *out);
void fkx_promote_result_free(fkx_promote_result *r);

/* Undo the latest promote into "name". If the target is exactly where the promote left it and nothing is open on it, it is reset to
 * the state before (exact); otherwise the inverse of the promote is replayed onto it (work done since is kept; a conflict aborts). */
int fkx_undo(fkx *x, const char *name, fkx_promote_result *out);

/* ---- stepwise primitives: everything the driver does except the session calls themselves, for hosts that bring their own ---- */

typedef struct fkx_diff_handle fkx_diff_handle;
/* A read transaction on "ref" with "against" attached as schema "other". *db is that connection; tables lists every table with its
 * status. The host runs its own session diff on the COMPARED tables and then closes the handle. */
int fkx_diff_open(fkx *x, const char *ref, const char *against, fkx_diff_handle **h, sqlite3 **db, const fkx_table_diff **tables, size_t *ntables, int *schema_differs);
void fkx_diff_close(fkx_diff_handle *h);

typedef struct fkx_replay fkx_replay;
/* Begin replaying the change old -> new onto "target": checks the schema policy for the tables named in touched[] (the host takes them from
 * its changeset), then returns *db in an open write transaction with foreign_keys on and the target's triggers on the touched tables
 * dropped. The host applies its changeset on *db without a savepoint of its own (SQLITE_CHANGESETAPPLY_NOSAVEPOINT) and calls
 * fkx_replay_commit (re-creates the triggers, commits: one store commit, then records the undo point) or fkx_replay_abort (rolls back,
 * which also puts the triggers back). A forgotten handle is rolled back by fkx_close. */
int fkx_replay_begin(fkx *x, const char *old_ref, const char *new_ref, const char *target, const char *const *touched, size_t ntouched, fkx_replay **r, sqlite3 **db, char **refusal);
int fkx_replay_commit(fkx_replay *r);
void fkx_replay_abort(fkx_replay *r);

#ifdef __cplusplus
}
#endif
#endif
