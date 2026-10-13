/* fkx without the session extension: the operations that need SQLite's session API say so. The session driver (a later change) replaces this
 * file in a build whose SQLite has SQLITE_ENABLE_SESSION. */
#include <stdlib.h>
#include <string.h>
#include "fkx.h"

int fkx_have_session(void) { return 0; }
int fkx_diff(fkx *x, const char *ref, const char *against, fkx_diff_result *out) { (void)x; (void)ref; (void)against; if (out) memset(out, 0, sizeof *out); return FKX_ENOSESSION; }
void fkx_diff_result_free(fkx_diff_result *r) {
  if (!r) return;
  for (size_t i = 0; i < r->ntables; i++) free(r->tables[i].name);
  free(r->tables);
  free(r->changeset);
  memset(r, 0, sizeof *r);
}
void fkx_promote_result_free(fkx_promote_result *r) {
  if (!r) return;
  for (size_t i = 0; i < r->nconflicts; i++) { free(r->conflicts[i].table); free(r->conflicts[i].key); }
  free(r->conflicts);
  free(r->refusal);
  memset(r, 0, sizeof *r);
}
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
