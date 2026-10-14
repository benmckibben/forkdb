/* fkx internals shared with the tests. Not part of the API. */
#ifndef FKX_INTERNAL_H
#define FKX_INTERNAL_H
#include "fkx.h"

/* one branch's metadata, decoded (see docs/FKX-DESIGN.md) */
typedef struct {
  fkx_kind kind;
  int ro;
  unsigned nlen, nundo;
  uint64_t uid, origin, base, owner;
  char name[FKX_NAME_MAX + 1];
  uint64_t undo[FKX_UNDO_KEEP_MAX][2]; /* newest first: before, after */
} Meta;

/* What the session driver (or its stand-in) needs from the core. */
int fkx_i_fail(fkx *x, int code, const char *fmt, ...);
int fkx_i_resolve(fkx *x, const char *ref, uint32_t *id, Meta *m);
int fkx_i_find_uid(fkx *x, uint64_t uid, uint32_t *id, Meta *m);
uint32_t fkx_i_refs(fkx *x, uint32_t id);
/* The handle's lock (the VFS's one mutex, recursive): held by every public operation for its whole duration. fkx_session.c wraps its own entry points with it. */
void fkx_i_lock(fkx *x);
void fkx_i_unlock(fkx *x);
int fkx_i_undo_before(fkx *x, uint32_t pid, const Meta *pm, uint64_t *ub);
int fkx_i_undo_after(fkx *x, uint32_t pid, const Meta *pm, uint64_t ub, int *recorded);
int fkx_i_discard_uid(fkx *x, uint64_t uid);
unsigned fkx_i_undo_keep(const fkx *x);
void fkx_i_ref(uint64_t uid, char out[24]);
int fkx_i_track(fkx *x, void *h, void (*fn)(void *));
void fkx_i_untrack(fkx *x, void *h);
/* The two operations that need a replay. Both return FKX_ENOSESSION without the driver; the core turns that into the right answer. */
int fkx_i_replay_promote(fkx *x, uint32_t fid, const Meta *fm, uint32_t pid, const Meta *pm, uint32_t bid, fkx_promote_result *out);
int fkx_i_replay_undo(fkx *x, uint32_t pid, const Meta *pm, fkx_promote_result *out);

/* Like fkx_open, over an io the caller owns (a simulated disk in the power-loss tests). fkx_close does not close or trim it. */
int fkx_open_io(const fk_io *io, const fkx_options *o, fkx **out);
/* The store, writable. For tests only: writing to it behind fkx's back breaks its invariants. */
fk_store *fkx_store_rw(fkx *x);
/* The VFS, for tests. */
struct FkVfs;
struct FkVfs *fkx_vfs(fkx *x);
/* Checks the invariants of the branch index; returns 0 if they hold, else a message in buf. For tests (the power-loss harness runs it after
 * every recovery): unique names and uids, one root, every fork's base alive and owned by it, every undo entry's two snapshots alive and
 * owned by it, no base or undo point that its owner does not list. */
int fkx_check(fkx *x, char *buf, size_t n);
#endif
