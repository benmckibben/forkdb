/* Force-included (gcc -include) into every file of the loadable-extension build, see scripts/build-ext.sh.
 *
 * It routes every sqlite3_* call in forkdb.so (the VFS, the branch layer, the extension itself) through the sqlite3_api_routines table that
 * the host passes to the init function. The .so then has no undefined sqlite3_* symbols and loads into any host SQLite: the system
 * libsqlite3 behind Python's sqlite3 module (opened RTLD_LOCAL, so its symbols are not visible to a library loaded later), or a SQLite linked
 * statically into a Node addon. Without it the load fails with 'undefined symbol: sqlite3_vfs_register' anywhere but the sqlite3 shell.
 *
 * The session API (sqlite3session_*, sqlite3changeset_*) is not in that table, so this build uses fkx/fkx_nosession.c. */
#ifndef FK_EXT_SHIM_H
#define FK_EXT_SHIM_H
#include "sqlite3ext.h"
extern const sqlite3_api_routines *sqlite3_api; /* defined once, by SQLITE_EXTENSION_INIT1 in ext/forkdb_ext.c */
#endif
