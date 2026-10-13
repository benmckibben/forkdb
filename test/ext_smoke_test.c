/* Smoke test for the forkdb extension: loads forkdb.so via dlopen/dlsym,
 * calls the init function, then exercises every SQL function.
 * The SQLite used here does NOT have SQLITE_OMIT_LOAD_EXTENSION.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <assert.h>
#include "sqlite3.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <forkdb.so>\n", argv[0]); return 1; }

    /* dlopen the extension */
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    typedef int (*init_fn)(sqlite3 *, char **, const sqlite3_api_routines *);
    init_fn forkdb_init = (init_fn)dlsym(h, "sqlite3_forkdb_init");
    if (!forkdb_init) { fprintf(stderr, "dlsym: %s\n", dlerror()); dlclose(h); return 1; }

    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    assert(rc == SQLITE_OK);

    /* Enable extension loading so SQLITE_EXTENSION_INIT2 works */
    rc = sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, NULL);
    assert(rc == SQLITE_OK);

    /* sqlite3_auto_extension needs the api_routines, which are internal.
     * Instead, call the real extension loader path via sqlite3_load_extension. */
    char *err = NULL;
    rc = sqlite3_load_extension(db, argv[1], "sqlite3_forkdb_init", &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "load failed (rc=%d): %s\n", rc, err ? err : "unknown"); sqlite3_free(err); return 1; }
    printf("extension loaded ok\n"); fflush(stdout);
    dlclose(h);

    /* forkdb_info virtual table */
    printf("forkdb_info:\n");
    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT key, value FROM forkdb_info ORDER BY rowid", -1, &st, NULL);
    if (rc != SQLITE_OK) { fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db)); return 1; }
    while (sqlite3_step(st) == SQLITE_ROW)
        printf("  %s = %s\n", sqlite3_column_text(st, 0), sqlite3_column_text(st, 1));
    sqlite3_finalize(st);

    /* Create a real temp store */
    char real_path[] = "/tmp/forkdb-ext-test.XXXXXX.fk";
    int fd = mkstemps(real_path, 3);
    assert(fd >= 0);
    close(fd);
    unlink(real_path);

    char sql[512];

    snprintf(sql, sizeof sql, "SELECT forkdb_init('%s', 'main')", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "init: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_fork('%s', 'main', 'test')", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "fork: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_snapshot('%s', 'main', 'snap1')", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "snapshot: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_vfs_name('%s')", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "vfs_name: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_diff('%s', 'test')", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "diff: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_promote('%s', 'test', NULL, 1)", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "promote dry: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_promote('%s', 'test', NULL, 0)", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "promote: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_undo('%s', 'main')", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "undo: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_discard('%s', 'test')", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "discard: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_reset('%s', 'main', 'snap1')", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "reset: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_compact('%s')", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "compact: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_open_db('%s', 'main', 0)", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "open_db: %s\n", err); sqlite3_free(err); return 1; }

    snprintf(sql, sizeof sql, "SELECT forkdb_close('%s')", real_path);
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "close: %s\n", err); sqlite3_free(err); return 1; }

    printf("ALL EXTENSION SMOKE TESTS PASSED\n");
    fflush(stdout);

    unlink(real_path);
    sqlite3_close(db);
    return 0;
}
