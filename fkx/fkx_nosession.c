/* fkx without the session extension: the operations that need SQLite's session API say so. The session driver (a later change) replaces this
 * file in a build whose SQLite has SQLITE_ENABLE_SESSION. */
#include <stdlib.h>
#include <string.h>
#include "fkx_internal.h"

int fkx_have_session(void) { return 0; }
int fkx_diff(fkx *x, const char *ref, const char *against, fkx_diff_result *out) { (void)x; (void)ref; (void)against; if (out) memset(out, 0, sizeof *out); return FKX_ENOSESSION; }
int fkx_diff_open(fkx *x, const char *ref, const char *against, fkx_diff_handle **h, sqlite3 **db, const fkx_table_diff **tables, size_t *ntables, int *schema_differs) {
  (void)x; (void)ref; (void)against; (void)h; (void)db; (void)tables; (void)ntables; (void)schema_differs;
  return FKX_ENOSESSION;
}
void fkx_diff_close(fkx_diff_handle *h) { (void)h; }
int fkx_replay_begin(fkx *x, const char *old_ref, const char *new_ref, const char *target, const char *const *touched, size_t ntouched, fkx_replay **r, sqlite3 **db, char **refusal) {
  (void)x; (void)old_ref; (void)new_ref; (void)target; (void)touched; (void)ntouched; (void)r; (void)db; (void)refusal;
  return FKX_ENOSESSION;
}
int fkx_replay_commit(fkx_replay *r) { (void)r; return FKX_ENOSESSION; }
void fkx_replay_abort(fkx_replay *r) { (void)r; }

int fkx_i_replay_promote(fkx *x, uint32_t fid, const Meta *fm, uint32_t pid, const Meta *pm, uint32_t bid, fkx_promote_result *out) {
  (void)fid; (void)pid; (void)bid;
  out->method = FKX_METHOD_REPLAY;
  return fkx_i_fail(x, FKX_ENOSESSION, "'%s' has moved since '%s' was forked, or has a connection open: promoting needs a replay, which needs the session extension (this build has none)", pm->name, fm->name);
}
int fkx_i_replay_undo(fkx *x, uint32_t pid, const Meta *pm, fkx_promote_result *out) {
  (void)pid; (void)out;
  return fkx_i_fail(x, FKX_ENOSESSION, "'%s' has changed since the promote, or has a connection open: undoing it needs a replay, which needs the session extension (this build has none)", pm->name);
}
