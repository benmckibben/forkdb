/* fkx internals shared with the tests. Not part of the API. */
#ifndef FKX_INTERNAL_H
#define FKX_INTERNAL_H
#include "fkx.h"

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
