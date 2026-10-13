/* forkdb SQLite extension: exposes fkx operations as SQL functions.
 * Loaded with .load forkdb (or sqlite3_auto_extension("forkdb_init")).
 *
 * At load time it checks whether the host exposes the session API by looking
 * for sqlite3session_create in the process. On hosts where it's absent,
 * forkdb_info.session_api is 0 and the diff/promote/undo functions return
 * an error naming the missing piece. The VFS and branch operations work
 * everywhere.
 *
 * Built without a SQLite of its own (uses sqlite3ext.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

/* sqlite3ext.h must be first */
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include "../fkx/fkx.h"

/* ---- registry: path -> fkx* ---- */

#define MAX_REPOS 16

static struct {
    char *path;
    fkx *x;
} repo_reg[MAX_REPOS];
static int nrepos = 0;

static fkx *repo_find(const char *path) {
    for (int i = 0; i < nrepos; i++)
        if (repo_reg[i].path && !strcmp(repo_reg[i].path, path))
            return repo_reg[i].x;
    return NULL;
}

__attribute__((unused)) __attribute__((unused)) static void repo_close_all(void) {
    for (int i = 0; i < nrepos; i++) {
        if (repo_reg[i].x) {
            fkx_close(repo_reg[i].x);
            free(repo_reg[i].path);
        }
    }
    nrepos = 0;
}

static int repo_add(const char *path, fkx *x, sqlite3_context *ctx) {
    int slot = -1;
    for (int i = 0; i < nrepos; i++) {
        if (!repo_reg[i].path) { slot = i; break; }
    }
    if (slot < 0) {
        if (nrepos >= MAX_REPOS) {
            sqlite3_result_error(ctx, "too many open repositories", -1);
            fkx_close(x);
            return 0;
        }
        slot = nrepos++;
    }
    repo_reg[slot].path = strdup(path);
    repo_reg[slot].x = x;
    return 1;
}

/* ---- session API detection ---- */

static int session_available(void) {
    return fkx_have_session();
}

/* ---- forkdb_info virtual table ---- */

typedef struct {
    sqlite3_vtab base;
} forkdb_info_vtab;

typedef struct {
    sqlite3_vtab_cursor base;
    int row;
} forkdb_info_cursor;

static int info_connect(sqlite3 *db, void *pAux, int argc, const char *const *argv,
                        sqlite3_vtab **ppVtab, char **pzErr) {
    (void)pAux; (void)argc; (void)argv;
    int rc = sqlite3_declare_vtab(db, "CREATE TABLE forkdb_info(key TEXT, value TEXT)");
    if (rc != SQLITE_OK) { *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db)); return rc; }
    forkdb_info_vtab *v = sqlite3_malloc(sizeof *v);
    if (!v) return SQLITE_NOMEM;
    memset(v, 0, sizeof *v);
    *ppVtab = &v->base;
    return SQLITE_OK;
}

static int info_disconnect(sqlite3_vtab *pVtab) {
    sqlite3_free(pVtab);
    return SQLITE_OK;
}

static int info_open(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor) {
    (void)pVtab;
    forkdb_info_cursor *c = sqlite3_malloc(sizeof *c);
    if (!c) return SQLITE_NOMEM;
    memset(c, 0, sizeof *c);
    *ppCursor = &c->base;
    return SQLITE_OK;
}

static int info_close(sqlite3_vtab_cursor *cur) {
    sqlite3_free(cur);
    return SQLITE_OK;
}

static int info_filter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr,
                       int argc, sqlite3_value **argv) {
    (void)idxNum; (void)idxStr; (void)argc; (void)argv;
    ((forkdb_info_cursor *)cur)->row = 0;
    return SQLITE_OK;
}

static int info_next(sqlite3_vtab_cursor *cur) {
    ((forkdb_info_cursor *)cur)->row++;
    return SQLITE_OK;
}

static int info_eof(sqlite3_vtab_cursor *cur) {
    return ((forkdb_info_cursor *)cur)->row >= 3;
}

static int info_column(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i) {
    forkdb_info_cursor *c = (forkdb_info_cursor *)cur;
    switch (c->row) {
        case 0:
            if (i == 0) sqlite3_result_text(ctx, "session_api", -1, SQLITE_STATIC);
            else if (i == 1) sqlite3_result_int(ctx, session_available());
            break;
        case 1:
            if (i == 0) sqlite3_result_text(ctx, "vfs", -1, SQLITE_STATIC);
            else if (i == 1) sqlite3_result_text(ctx, "fkvfs", -1, SQLITE_STATIC);
            break;
        case 2:
            if (i == 0) sqlite3_result_text(ctx, "version", -1, SQLITE_STATIC);
            else if (i == 1) sqlite3_result_text(ctx, "0.1.0", -1, SQLITE_STATIC);
            break;
    }
    return SQLITE_OK;
}

static int info_rowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid) {
    *pRowid = ((forkdb_info_cursor *)cur)->row;
    return SQLITE_OK;
}

static int info_best_index(sqlite3_vtab *pVtab, sqlite3_index_info *pIdxInfo) {
    (void)pVtab;
    pIdxInfo->estimatedCost = 1.0;
    pIdxInfo->estimatedRows = 3;
    return SQLITE_OK;
}

static sqlite3_module forkdb_info_module = {
    .iVersion = 0,
    .xCreate = NULL,          /* eponymous: no CREATE VIRTUAL TABLE needed */
    .xConnect = info_connect,
    .xBestIndex = info_best_index,
    .xDisconnect = info_disconnect,
    .xDestroy = info_disconnect,
    .xOpen = info_open,
    .xClose = info_close,
    .xFilter = info_filter,
    .xNext = info_next,
    .xEof = info_eof,
    .xColumn = info_column,
    .xRowid = info_rowid
};

/* ---- helper: open a repo from a function argument ---- */

static fkx *repo_from_arg(sqlite3_context *ctx, int argc, sqlite3_value **argv, int *out_rc) {
    if (argc < 1) {
        sqlite3_result_error(ctx, "need a store path", -1);
        *out_rc = FKX_EINVAL;
        return NULL;
    }
    const char *path = (const char *)sqlite3_value_text(argv[0]);
    if (!path) {
        sqlite3_result_error(ctx, "store path is NULL", -1);
        *out_rc = FKX_EINVAL;
        return NULL;
    }
    fkx *x = repo_find(path);
    if (x) { *out_rc = FKX_OK; return x; }

    fkx_options o;
    fkx_options_default(&o);
    o.create = 0;
    int rc = fkx_open(path, &o, &x);
    if (rc != FKX_OK) {
        sqlite3_result_error(ctx, fkx_errmsg(x), -1);
        fkx_close(x);
        *out_rc = rc;
        return NULL;
    }
    if (!repo_add(path, x, ctx)) {
        *out_rc = FKX_ENOMEM;
        return NULL;
    }
    *out_rc = FKX_OK;
    return x;
}

/* ---- SQL functions ---- */

static void fn_init(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc < 1) { sqlite3_result_error(ctx, "init: need a path", -1); return; }
    const char *path = (const char *)sqlite3_value_text(argv[0]);
    if (!path) { sqlite3_result_error(ctx, "init: path is NULL", -1); return; }
    const char *name = argc >= 2 ? (const char *)sqlite3_value_text(argv[1]) : "main";
    if (!name) name = "main";

    fkx *x = repo_find(path);
    if (x) { sqlite3_result_error(ctx, "store is already open", -1); return; }

    fkx_options o;
    fkx_options_default(&o);
    o.create = 1;
    o.root_name = name;
    int rc = fkx_open(path, &o, &x);
    if (rc != FKX_OK) { sqlite3_result_error(ctx, fkx_errmsg(x), -1); fkx_close(x); return; }
    if (!repo_add(path, x, ctx)) return;

    sqlite3_result_null(ctx);
}

static void fn_close(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc < 1) { sqlite3_result_error(ctx, "close: need a path", -1); return; }
    const char *path = (const char *)sqlite3_value_text(argv[0]);
    if (!path) { sqlite3_result_error(ctx, "close: path is NULL", -1); return; }

    for (int i = 0; i < nrepos; i++) {
        if (repo_reg[i].path && !strcmp(repo_reg[i].path, path)) {
            fkx_close(repo_reg[i].x);
            free(repo_reg[i].path);
            repo_reg[i].path = NULL;
            repo_reg[i].x = NULL;
            while (nrepos > 0 && !repo_reg[nrepos - 1].path)
                nrepos--;
            sqlite3_result_null(ctx);
            return;
        }
    }
    sqlite3_result_error(ctx, "store not open", -1);
}

static void fn_fork(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    int rc; fkx *x = repo_from_arg(ctx, argc, argv, &rc);
    if (!x) return;
    if (!session_available() && argc >= 1) {
        /* fork itself doesn't need session, so this works */
    }
    const char *from = argc >= 2 ? (const char *)sqlite3_value_text(argv[1]) : "main";
    const char *name = argc >= 3 ? (const char *)sqlite3_value_text(argv[2]) : NULL;
    if (!from || !name) { sqlite3_result_error(ctx, "fork: need from and name", -1); return; }

    rc = fkx_fork(x, from, name);
    if (rc != FKX_OK) { sqlite3_result_error(ctx, fkx_errmsg(x), -1); return; }

    fkx_info info;
    rc = fkx_info_get(x, name, &info);
    if (rc != FKX_OK) { sqlite3_result_error(ctx, fkx_errmsg(x), -1); return; }
    sqlite3_result_int64(ctx, (sqlite3_int64)info.uid);
}

static void fn_snapshot(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    int rc; fkx *x = repo_from_arg(ctx, argc, argv, &rc);
    if (!x) return;
    const char *from = argc >= 2 ? (const char *)sqlite3_value_text(argv[1]) : "main";
    const char *name = argc >= 3 ? (const char *)sqlite3_value_text(argv[2]) : NULL;
    if (!from || !name) { sqlite3_result_error(ctx, "snapshot: need from and name", -1); return; }

    rc = fkx_snapshot(x, from, name);
    if (rc != FKX_OK) { sqlite3_result_error(ctx, fkx_errmsg(x), -1); return; }

    fkx_info info;
    rc = fkx_info_get(x, name, &info);
    if (rc != FKX_OK) { sqlite3_result_error(ctx, fkx_errmsg(x), -1); return; }
    sqlite3_result_int64(ctx, (sqlite3_int64)info.uid);
}

static void fn_discard(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    int rc; fkx *x = repo_from_arg(ctx, argc, argv, &rc);
    if (!x) return;
    const char *name = argc >= 2 ? (const char *)sqlite3_value_text(argv[1]) : NULL;
    if (!name) { sqlite3_result_error(ctx, "discard: need a name", -1); return; }

    rc = fkx_discard(x, name);
    if (rc != FKX_OK) { sqlite3_result_error(ctx, fkx_errmsg(x), -1); return; }
    sqlite3_result_null(ctx);
}

static void fn_reset(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    int rc; fkx *x = repo_from_arg(ctx, argc, argv, &rc);
    if (!x) return;
    const char *name = argc >= 2 ? (const char *)sqlite3_value_text(argv[1]) : NULL;
    const char *to = argc >= 3 ? (const char *)sqlite3_value_text(argv[2]) : NULL;
    if (!name) { sqlite3_result_error(ctx, "reset: need a name", -1); return; }
    /* to == NULL is valid: fkx_reset treats it as "reset to this branch's base". */

    rc = fkx_reset(x, name, to);
    if (rc != FKX_OK) { sqlite3_result_error(ctx, fkx_errmsg(x), -1); return; }
    sqlite3_result_null(ctx);
}

static void fn_compact(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    int rc; fkx *x = repo_from_arg(ctx, argc, argv, &rc);
    if (!x) return;
    uint64_t min_bytes = 0;
    if (argc >= 2) min_bytes = (uint64_t)sqlite3_value_int64(argv[1]);

    int did = 0;
    rc = fkx_compact_if_needed(x, min_bytes, &did);
    if (rc != FKX_OK) { sqlite3_result_error(ctx, fkx_errmsg(x), -1); return; }
    sqlite3_result_int(ctx, did);
}

static void fn_vfs_name(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    int rc; fkx *x = repo_from_arg(ctx, argc, argv, &rc);
    if (!x) return;
    sqlite3_result_text(ctx, fkx_vfs_name(x), -1, SQLITE_STATIC);
}

/* ---- session-dependent functions ---- */

/* Append formatted text that fits entirely. snprintf's return is the length that
 * would have been written, so a truncated call must not advance the cursor.
 * Returns the new offset, or -1 if the text does not fit (buffer restored). */
static int append_fit(char *buf, int cap, int off, const char *fmt, ...) {
    if (off < 0 || off >= cap) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, (size_t)(cap - off), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= cap - off) {
        buf[off] = '\0';
        return -1;
    }
    return off + n;
}

static int check_session(sqlite3_context *ctx) {
    if (session_available()) return 1;
    sqlite3_result_error(ctx, "this host has no session API: diff, promote and undo are not available (see forkdb_info)", -1);
    return 0;
}

static void fn_diff(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (!check_session(ctx)) return;
    int rc; fkx *x = repo_from_arg(ctx, argc, argv, &rc);
    if (!x) return;
    const char *ref = argc >= 2 ? (const char *)sqlite3_value_text(argv[1]) : NULL;
    const char *against = argc >= 3 ? (const char *)sqlite3_value_text(argv[2]) : NULL;
    if (!ref) { sqlite3_result_error(ctx, "diff: need a ref", -1); return; }

    fkx_diff_result r;
    memset(&r, 0, sizeof r);
    rc = fkx_diff(x, ref, against, &r);
    if (rc != FKX_OK) { sqlite3_result_error(ctx, fkx_errmsg(x), -1); return; }

    /* JSON text. cap leaves room for the closing ]} and a NUL after the
     * last table entry that fit completely. */
    char buf[4096];
    const int cap = (int)sizeof buf - 2;
    int off = append_fit(buf, cap, 0, "{\"schema_differs\":%s,\"nchangeset\":%d,\"tables\":[",
                         r.schema_differs ? "true" : "false", r.nchangeset);
    for (size_t i = 0; i < r.ntables && off >= 0; i++) {
        fkx_table_diff *t = &r.tables[i];
        const char *st = "?";
        switch (t->status) {
            case FKX_TABLE_COMPARED: st = "compared"; break;
            case FKX_TABLE_NO_PK: st = "no_pk"; break;
            case FKX_TABLE_ONLY_HERE: st = "only_here"; break;
            case FKX_TABLE_ONLY_THERE: st = "only_there"; break;
            case FKX_TABLE_SCHEMA: st = "schema"; break;
            case FKX_TABLE_VIRTUAL: st = "virtual"; break;
        }
        int next;
        if (t->status == FKX_TABLE_COMPARED)
            next = append_fit(buf, cap, off,
                              "%s{\"name\":\"%s\",\"status\":\"%s\",\"inserts\":%" PRIu64 ",\"updates\":%" PRIu64 ",\"deletes\":%" PRIu64 "}",
                              i > 0 ? "," : "", t->name, st, t->inserts, t->updates, t->deletes);
        else if (t->status == FKX_TABLE_NO_PK)
            next = append_fit(buf, cap, off,
                              "%s{\"name\":\"%s\",\"status\":\"%s\",\"rows_differ\":%s}",
                              i > 0 ? "," : "", t->name, st, t->rows_differ ? "true" : "false");
        else
            next = append_fit(buf, cap, off,
                              "%s{\"name\":\"%s\",\"status\":\"%s\"}",
                              i > 0 ? "," : "", t->name, st);
        if (next < 0) break;
        off = next;
    }
    if (off < 0 || off > cap - 1) off = off < 0 ? 0 : cap - 1;
    buf[off] = ']';
    buf[off + 1] = '}';
    buf[off + 2] = '\0';
    sqlite3_result_text(ctx, buf, -1, SQLITE_TRANSIENT);

    /* also return changeset blob */
    /* Can't really do two return values from a scalar SQL function.
     * The changeset is available via forkdb_last_changeset() below. */
    fkx_diff_result_free(&r);
}

static void fn_promote(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (!check_session(ctx)) return;
    int rc; fkx *x = repo_from_arg(ctx, argc, argv, &rc);
    if (!x) return;
    const char *name = argc >= 2 ? (const char *)sqlite3_value_text(argv[1]) : NULL;
    if (!name) { sqlite3_result_error(ctx, "promote: need a name", -1); return; }

    fkx_promote_opts o;
    o.into = argc >= 3 ? (const char *)sqlite3_value_text(argv[2]) : NULL;
    o.dry_run = 0;
    if (argc >= 4) o.dry_run = sqlite3_value_int(argv[3]) != 0;

    fkx_promote_result r;
    memset(&r, 0, sizeof r);
    rc = fkx_promote(x, name, &o, &r);

    const char *method = r.method == FKX_METHOD_FAST_FORWARD ? "fast-forward" :
                          r.method == FKX_METHOD_REPLAY ? "replay" :
                          r.method == FKX_METHOD_UP_TO_DATE ? "up-to-date" : "none";

    char buf[4096];
    if (rc == FKX_EREFUSED && r.refusal) {
        snprintf(buf, sizeof buf, "refused: %s", r.refusal);
    } else if (rc == FKX_ECONFLICT) {
        snprintf(buf, sizeof buf, "conflict: %zu conflict(s), first: %s",
                 r.nconflicts, r.nconflicts > 0 && r.conflicts[0].table ? r.conflicts[0].table : "?");
    } else if (rc == FKX_OK) {
        snprintf(buf, sizeof buf, "%s%s%s", method,
                 r.undo_recorded ? " (undo point recorded)" : "",
                 r.dry_run ? " (dry run)" : "");
    } else {
        snprintf(buf, sizeof buf, "%s", fkx_errmsg(x));
    }
    sqlite3_result_text(ctx, buf, -1, SQLITE_TRANSIENT);
    fkx_promote_result_free(&r);
}

static void fn_undo(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (!check_session(ctx)) return;
    int rc; fkx *x = repo_from_arg(ctx, argc, argv, &rc);
    if (!x) return;
    const char *name = argc >= 2 ? (const char *)sqlite3_value_text(argv[1]) : NULL;
    if (!name) { sqlite3_result_error(ctx, "undo: need a name", -1); return; }

    fkx_promote_result r;
    memset(&r, 0, sizeof r);
    rc = fkx_undo(x, name, &r);

    const char *method = r.method == FKX_METHOD_FAST_FORWARD ? "exact" :
                          r.method == FKX_METHOD_REPLAY ? "replay" : "?";

    char buf[256];
    if (rc == FKX_OK)
        snprintf(buf, sizeof buf, "undone (%s)", method);
    else if (rc == FKX_EREFUSED && r.refusal)
        snprintf(buf, sizeof buf, "refused: %s", r.refusal);
    else if (rc == FKX_ECONFLICT)
        snprintf(buf, sizeof buf, "conflict: %zu conflict(s)", r.nconflicts);
    else
        snprintf(buf, sizeof buf, "%s", fkx_errmsg(x));

    sqlite3_result_text(ctx, buf, -1, SQLITE_TRANSIENT);
    fkx_promote_result_free(&r);
}

static void fn_open_db(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    int rc; fkx *x = repo_from_arg(ctx, argc, argv, &rc);
    if (!x) return;
    const char *ref = argc >= 2 ? (const char *)sqlite3_value_text(argv[1]) : "main";

    /* We can't return a sqlite3* handle to the user at the SQL level.
     * Instead provide the VFS URI as text so the user can ATTACH. */
    char uri[512];
    snprintf(uri, sizeof uri, "file:%s?vfs=%s", ref, fkx_vfs_name(x));
    sqlite3_result_text(ctx, uri, -1, SQLITE_TRANSIENT);
}

/* ---- registration ---- */

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_forkdb_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    (void)pzErrMsg;
    if (pApi) SQLITE_EXTENSION_INIT2(pApi);

    /* virtual table: forkdb_info */
    sqlite3_create_module(db, "forkdb_info", &forkdb_info_module, NULL);

    /* branch operations (work everywhere) */
    int flags = SQLITE_UTF8 | SQLITE_DIRECTONLY;
    sqlite3_create_function(db, "forkdb_init",     -1, flags, NULL, fn_init,     NULL, NULL);
    sqlite3_create_function(db, "forkdb_close",    -1, flags, NULL, fn_close,    NULL, NULL);
    sqlite3_create_function(db, "forkdb_fork",     -1, flags, NULL, fn_fork,     NULL, NULL);
    sqlite3_create_function(db, "forkdb_snapshot", -1, flags, NULL, fn_snapshot, NULL, NULL);
    sqlite3_create_function(db, "forkdb_discard",  -1, flags, NULL, fn_discard,  NULL, NULL);
    sqlite3_create_function(db, "forkdb_reset",    -1, flags, NULL, fn_reset,    NULL, NULL);
    sqlite3_create_function(db, "forkdb_vfs_name", -1, flags, NULL, fn_vfs_name, NULL, NULL);
    sqlite3_create_function(db, "forkdb_compact",  -1, flags, NULL, fn_compact,  NULL, NULL);

    /* session-dependent operations */
    sqlite3_create_function(db, "forkdb_diff",     -1, flags, NULL, fn_diff,     NULL, NULL);
    sqlite3_create_function(db, "forkdb_promote",  -1, flags, NULL, fn_promote,  NULL, NULL);
    sqlite3_create_function(db, "forkdb_undo",     -1, flags, NULL, fn_undo,     NULL, NULL);
    sqlite3_create_function(db, "forkdb_open_db",  -1, flags, NULL, fn_open_db,  NULL, NULL);

    return SQLITE_OK;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_extension_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    return sqlite3_forkdb_init(db, pzErrMsg, pApi);
}
