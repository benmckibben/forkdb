/* forkdb CLI: the command-line tool. Built with its own SQLite (the session variant from vfs/build.sh,
 * -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK), it works offline on a store no application has open.
 * Subcommands: init, fork, snapshot, list, info, diff, promote, undo, discard, reset, gc, compact, sql, shell.
 *
 * Output: human-readable on stdout; --json for structured output on stdout. Errors on stderr, exit code 1.
 *
 * The shell subcommand uses SQLite's built-in shell (the amalgamation includes it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <inttypes.h>

#include "../fkx/fkx.h"
#include "../store/fkstore.h"

#define PROGRAM "forkdb"

#define STRLIT(s) s, sizeof(s)-1

static int json_output = 0;

static void die(const char *msg) {
    fprintf(stderr, PROGRAM ": %s\n", msg);
    if (ferror(stderr)) exit(1);
}


static void die_err(const char *what, const fkx *x, int rc) {
    fprintf(stderr, PROGRAM ": %s: %s", what, fkx_errmsg(x));
    if (rc == FKX_EREFUSED) fprintf(stderr, " (refused)");
    else if (rc == FKX_ECONFLICT) fprintf(stderr, " (conflict)");
    fputc('\n', stderr);
    if (ferror(stderr)) exit(1);
}

/* fkx_open frees its handle when it fails and leaves *x NULL, so there is no fkx_errmsg to ask (die_err(NULL) crashed): say what happened
 * from the code. Same wording as open_failed in ext/forkdb_ext.c. */
static void open_failed(const char *what, const char *path, int rc) {
    fprintf(stderr, PROGRAM ": %s: ", what);
    switch (rc) {
        case FKX_ENOENT:   fprintf(stderr, "no store at '%s'", path); break;
        case FKX_ESTORE:   fprintf(stderr, "cannot open the store file '%s'", path); break;
        case FKX_EVERSION: fprintf(stderr, "'%s' is a store older than format version 4", path); break;
        case FKX_ECORRUPT: fprintf(stderr, "'%s' is damaged or is not a forkdb store", path); break;
        case FKX_ELOCKED:  fprintf(stderr, "the store '%s' is open in another process (or already open here): one process at a time", path); break;
        case FKX_ENOMEM:   fprintf(stderr, "out of memory"); break;
        default:           fprintf(stderr, "opening '%s' failed (code %d)", path, rc); break;
    }
    fputc('\n', stderr);
    if (ferror(stderr)) exit(1);
}

/* ---- json helpers ---- */

static void json_str(FILE *fp, const char *s) {
    if (!s) { fputs("null", fp); return; }
    fputc('"', fp);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { fputc('\\', fp); fputc(*s, fp); }
        else if (*s == '\n') fputs("\\n", fp);
        else if (*s == '\t') fputs("\\t", fp);
        else if (*s == '\r') fputs("\\r", fp);
        else fputc(*s, fp);
    }
    fputc('"', fp);
}

static void json_nl(FILE *fp) { if (!json_output) return; fputc('\n', fp); }

/* ---- subcommand dispatcher ---- */

static int cmd_init(int argc, char **argv);
static int cmd_fork(int argc, char **argv);
static int cmd_snapshot(int argc, char **argv);
static int cmd_list(int argc, char **argv);
static int cmd_info(int argc, char **argv);
static int cmd_diff(int argc, char **argv);
static int cmd_promote(int argc, char **argv);
static int cmd_undo(int argc, char **argv);
static int cmd_discard(int argc, char **argv);
static int cmd_reset(int argc, char **argv);
static int cmd_gc(int argc, char **argv);
static int cmd_compact(int argc, char **argv);
static int cmd_sql(int argc, char **argv);
static int cmd_shell(int argc, char **argv);

typedef struct { const char *name; int (*fn)(int, char **); const char *summary; } subcmd;
static const subcmd commands[] = {
    {"init",     cmd_init,     "Create a new store with one branch (\"main\")"},
    {"fork",     cmd_fork,     "Fork a branch"},
    {"snapshot", cmd_snapshot, "Take a named snapshot"},
    {"list",     cmd_list,     "List branches"},
    {"info",     cmd_info,     "Show one branch's details"},
    {"diff",     cmd_diff,     "Logical diff between two branches"},
    {"promote",  cmd_promote,  "Promote a branch into its target"},
    {"undo",     cmd_undo,     "Undo the latest promote into a branch"},
    {"discard",  cmd_discard,  "Discard a branch and its history"},
    {"reset",    cmd_reset,    "Reset a branch to another's state"},
    {"gc",       cmd_gc,       "Garbage-collect dead branches"},
    {"compact",  cmd_compact,  "Compact the store"},
    {"sql",      cmd_sql,      "Run SQL against a branch"},
    {"shell",    cmd_shell,    "Open an interactive SQLite shell on the store"},
    {NULL, NULL, NULL}
};

static void print_help(FILE *fp) {
    fprintf(fp, "usage: " PROGRAM " <command> [options]\n\n"
            "forkdb works offline on a store no application has open (single-writer).\n\n"
            "Commands:\n");
    int w = 0;
    for (const subcmd *sc = commands; sc->name; sc++) {
        int l = (int)strlen(sc->name);
        if (l > w) w = l;
    }
    for (const subcmd *sc = commands; sc->name; sc++)
        fprintf(fp, "  %-*s  %s\n", w, sc->name, sc->summary);
    fprintf(fp, "\nGlobal options:\n"
            "  --json        structured output on stdout\n"
            "  --help, -h    this message\n");
}

static int open_store(const char *path, int create, fkx **out) {
    fkx_options o;
    fkx_options_default(&o);
    o.create = create;
    return fkx_open(path, &o, out);
}

static void print_conflicts(fkx_promote_result *r) {
    for (size_t i = 0; i < r->nconflicts; i++) {
        fkx_conflict *c = &r->conflicts[i];
        const char *kind = "?";
        switch (c->kind) {
            case 1: kind = "DATA"; break;
            case 2: kind = "NOTFOUND"; break;
            case 3: kind = "CONFLICT"; break;
            case 4: kind = "CONSTRAINT"; break;
            case 5: kind = "FOREIGN_KEY"; break;
        }
        printf("  table \"%s\": %s conflict, key \"%s\"%s\n",
               c->table ? c->table : "?", kind, c->key ? c->key : "?",
               i+1 < r->nconflicts ? "" : "");
    }
}

static void print_promote_result(fkx_promote_result *r, ssize_t nconflicts) {
    if (json_output) {
        printf("{\"method\":\"%s\",\"dry_run\":%s,\"undo_recorded\":%s",
               r->method == FKX_METHOD_FAST_FORWARD ? "fast-forward" :
               r->method == FKX_METHOD_REPLAY ? "replay" :
               r->method == FKX_METHOD_UP_TO_DATE ? "up-to-date" : "none",
               r->dry_run ? "true" : "false",
               r->undo_recorded ? "true" : "false");
        if (r->refusal) { printf(",\"refusal\":"); json_str(stdout, r->refusal); }
        if (nconflicts > 0) {
            printf(",\"nconflicts\":%zd,\"conflicts\":[", nconflicts);
            for (size_t i = 0; i < r->nconflicts; i++) {
                fkx_conflict *c = &r->conflicts[i];
                if (i > 0) fputc(',', stdout);
                printf("{\"table\":");
                json_str(stdout, c->table);
                printf(",\"kind\":%d,\"op\":%d,\"key\":", c->kind, c->op);
                json_str(stdout, c->key);
                fputc('}', stdout);
            }
            fputs("]", stdout);
        }
        fputs("}\n", stdout);
    } else {
        const char *method = r->method == FKX_METHOD_FAST_FORWARD ? "fast-forward" :
                             r->method == FKX_METHOD_REPLAY ? "replay" :
                             r->method == FKX_METHOD_UP_TO_DATE ? "up to date" : "?";
        printf("promoted (%s%s)\n", method,
               r->undo_recorded ? ", undo point recorded" :
               r->dry_run ? ", dry run" : "");
        if (r->refusal) printf("  refused: %s\n", r->refusal);
        if (nconflicts > 0) {
            printf("%zd conflict%s:\n", nconflicts, nconflicts == 1 ? "" : "s");
            print_conflicts(r);
        }
    }
}

/* ---- subcommand implementations ---- */

static int cmd_init(int argc, char **argv) {
    static struct option lopts[] = {
        {"name", required_argument, 0, 'n'},
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    const char *name = "main";
    int c;
    while ((c = getopt_long(argc, argv, "n:h", lopts, NULL)) != -1) {
        switch (c) {
            case 'n': name = optarg; break;
            case 'h': printf("usage: " PROGRAM " init <path> [--name=main]\n"); return 0;
            case 0: break;
            default: die("init: bad flag"); return 1;
        }
    }
    if (optind >= argc) { die("init: need a path"); return 1; }
    const char *path = argv[optind];

    fkx_options o;
    fkx_options_default(&o);
    o.create = 1;
    o.root_name = name;
    fkx *x = NULL;
    int rc = fkx_open(path, &o, &x);
    if (rc != FKX_OK) { open_failed("init", path, rc); fkx_close(x); return 1; }

    fkx_info info;
    rc = fkx_info_get(x, name, &info);
    if (rc != FKX_OK) { die_err("init", x, rc); fkx_close(x); return 1; }

    if (json_output) {
        printf("{\"status\":\"created\",\"branch\":");
        json_str(stdout, name);
        printf(",\"uid\":\"%016" PRIx64 "\"}\n", info.uid);
    }
    else printf("created store \"%s\", branch \"%s\"\n", path, name);

    fkx_close(x);
    return 0;
}

static int cmd_list(int argc, char **argv) {
    static struct option lopts[] = {
        {"all", no_argument, 0, 'a'},
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    int all = 0;
    int c;
    while ((c = getopt_long(argc, argv, "ah", lopts, NULL)) != -1) {
        switch (c) {
            case 'a': all = 1; break;
            case 'h': printf("usage: " PROGRAM " list <path> [--all] [--json]\n"); return 0;
            case 0: break;
            default: die("list: bad flag"); return 1;
        }
    }
    if (optind >= argc) { die("list: need a path"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("list", argv[optind], rc); fkx_close(x); return 1; }

    size_t n = 0;
    rc = fkx_list(x, all, NULL, 0, &n);
    if (rc != FKX_OK) { die_err("list", x, rc); fkx_close(x); return 1; }
    if (n == 0) { fkx_close(x); return 0; }

    fkx_info *infos = calloc(n, sizeof(fkx_info));
    if (!infos) { die("out of memory"); fkx_close(x); return 1; }
    size_t got = 0;
    rc = fkx_list(x, all, infos, n, &got);
    if (rc != FKX_OK) { die_err("list", x, rc); free(infos); fkx_close(x); return 1; }

    if (json_output) {
        fputs("[", stdout);
        for (size_t i = 0; i < got; i++) {
            if (i > 0) fputc(',', stdout);
            json_nl(stdout);
            fkx_info *inf = &infos[i];
            const char *kind = inf->kind == FKX_ROOT ? "root" :
                               inf->kind == FKX_FORK ? "fork" :
                               inf->kind == FKX_SNAPSHOT ? "snapshot" :
                               inf->kind == FKX_BASE ? "base" :
                               inf->kind == FKX_UNDO ? "undo" : "?";
            printf("{\"name\":");
            json_str(stdout, inf->name[0] ? inf->name : NULL);
            printf(",\"uid\":\"%016" PRIx64 "\",\"kind\":", inf->uid);
            json_str(stdout, kind);
            printf(",\"base_uid\":\"%016" PRIx64 "\",\"state\":%" PRIu64
                   ",\"size_pages\":%u,\"n_undo\":%u,\"ro\":%s}",
                   inf->base_uid, inf->state, inf->size_pages, inf->n_undo,
                   inf->read_only ? "true" : "false");
        }
        json_nl(stdout);
        fputs("]\n", stdout);
    } else {
        printf("%-32s  %-16s  %-10s  %6s  %6s  %s\n",
               "NAME", "UID", "KIND", "BASE", "SIZE", "RO");
        for (size_t i = 0; i < got; i++) {
            fkx_info *inf = &infos[i];
            const char *kind = inf->kind == FKX_ROOT ? "root" :
                               inf->kind == FKX_FORK ? "fork" :
                               inf->kind == FKX_SNAPSHOT ? "snapshot" :
                               inf->kind == FKX_BASE ? "base" :
                               inf->kind == FKX_UNDO ? "undo" : "?";
            char uid[17]; snprintf(uid, sizeof uid, "%016" PRIx64, inf->uid);
            printf("%-32s  %s  %-10s  %6u  %6u  %s\n",
                   inf->name[0] ? inf->name : kind, uid, kind,
                   (unsigned)(inf->base_uid & 0xFFFF),
                   inf->size_pages,
                   inf->read_only ? "yes" : "no");
        }
    }
    free(infos);
    fkx_close(x);
    return 0;
}

static int cmd_fork(int argc, char **argv) {
    static struct option lopts[] = {
        {"from", required_argument, 0, 'f'},
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    const char *from = "main";
    int c;
    while ((c = getopt_long(argc, argv, "f:h", lopts, NULL)) != -1) {
        switch (c) {
            case 'f': from = optarg; break;
            case 'h': printf("usage: " PROGRAM " fork <path> <name> [--from=main]\n"); return 0;
            case 0: break;
            default: die("fork: bad flag"); return 1;
        }
    }
    if (optind + 1 >= argc) { die("fork: need a path and a name"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("fork", argv[optind], rc); fkx_close(x); return 1; }

    rc = fkx_fork(x, from, argv[optind + 1]);
    if (rc != FKX_OK) { die_err("fork", x, rc); fkx_close(x); return 1; }

    fkx_info info;
    rc = fkx_info_get(x, argv[optind + 1], &info);
    if (rc != FKX_OK) { die_err("fork", x, rc); fkx_close(x); return 1; }

    if (json_output) {
        printf("{\"forked\":{\"name\":");
        json_str(stdout, argv[optind + 1]);
        printf(",\"from\":");
        json_str(stdout, from);
        printf(",\"uid\":\"%016" PRIx64 "\"}}\n", info.uid);
    }
    else printf("forked \"%s\" from \"%s\" (uid %016" PRIx64 ")\n",
                argv[optind + 1], from, info.uid);

    fkx_close(x);
    return 0;
}

static int cmd_snapshot(int argc, char **argv) {
    static struct option lopts[] = {
        {"from", required_argument, 0, 'f'},
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    const char *from = "main";
    int c;
    while ((c = getopt_long(argc, argv, "f:h", lopts, NULL)) != -1) {
        switch (c) {
            case 'f': from = optarg; break;
            case 'h': printf("usage: " PROGRAM " snapshot <path> <name> [--from=main]\n"); return 0;
            case 0: break;
            default: die("snapshot: bad flag"); return 1;
        }
    }
    if (optind + 1 >= argc) { die("snapshot: need a path and a name"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("snapshot", argv[optind], rc); fkx_close(x); return 1; }

    rc = fkx_snapshot(x, from, argv[optind + 1]);
    if (rc != FKX_OK) { die_err("snapshot", x, rc); fkx_close(x); return 1; }

    fkx_info info;
    rc = fkx_info_get(x, argv[optind + 1], &info);
    if (rc != FKX_OK) { die_err("snapshot", x, rc); fkx_close(x); return 1; }

    if (json_output) {
        printf("{\"snapshot\":{\"name\":");
        json_str(stdout, argv[optind + 1]);
        printf(",\"from\":");
        json_str(stdout, from);
        printf(",\"uid\":\"%016" PRIx64 "\"}}\n", info.uid);
    }
    else printf("snapshot \"%s\" from \"%s\" (uid %016" PRIx64 ")\n",
                argv[optind + 1], from, info.uid);

    fkx_close(x);
    return 0;
}

static int cmd_info(int argc, char **argv) {
    static struct option lopts[] = {
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", lopts, NULL)) != -1) {
        switch (c) {
            case 'h': printf("usage: " PROGRAM " info <path> [<ref>]\n"); return 0;
            case 0: break;
            default: die("info: bad flag"); return 1;
        }
    }
    if (optind >= argc) { die("info: need a path"); return 1; }
    const char *ref = optind + 1 < argc ? argv[optind + 1] : "main";

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("info", argv[optind], rc); fkx_close(x); return 1; }

    fkx_info info;
    rc = fkx_info_get(x, ref, &info);
    if (rc != FKX_OK) { die_err("info", x, rc); fkx_close(x); return 1; }

    const char *kind = info.kind == FKX_ROOT ? "root" :
                       info.kind == FKX_FORK ? "fork" :
                       info.kind == FKX_SNAPSHOT ? "snapshot" :
                       info.kind == FKX_BASE ? "base" :
                       info.kind == FKX_UNDO ? "undo" : "?";
    if (json_output) {
        printf("{\"name\":");
        json_str(stdout, info.name[0] ? info.name : NULL);
        printf(",\"uid\":\"%016" PRIx64 "\",\"kind\":", info.uid);
        json_str(stdout, kind);
        printf(",\"origin_uid\":\"%016" PRIx64 "\",\"base_uid\":\"%016" PRIx64
               "\",\"owner_uid\":\"%016" PRIx64 "\",\"state\":%" PRIu64
               ",\"size_pages\":%u,\"n_undo\":%u,\"open_connections\":%u,\"ro\":%s}\n",
               info.origin_uid, info.base_uid, info.owner_uid, info.state,
               info.size_pages, info.n_undo, info.open_connections,
               info.read_only ? "true" : "false");
    } else {
        printf("name:            %s\n", info.name[0] ? info.name : "(internal)");
        printf("uid:             %016" PRIx64 "\n", info.uid);
        printf("kind:            %s\n", kind);
        printf("origin_uid:      %016" PRIx64 "\n", info.origin_uid);
        printf("base_uid:        %016" PRIx64 "\n", info.base_uid);
        printf("owner_uid:       %016" PRIx64 "\n", info.owner_uid);
        printf("state:           %" PRIu64 "\n", info.state);
        printf("size_pages:      %u\n", info.size_pages);
        printf("n_undo:          %u\n", info.n_undo);
        printf("open_connections: %u\n", info.open_connections);
        printf("read_only:       %s\n", info.read_only ? "yes" : "no");
    }

    fkx_close(x);
    return 0;
}

static int cmd_diff(int argc, char **argv) {
    static struct option lopts[] = {
        {"against", required_argument, 0, 'a'},
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    const char *against = NULL;
    int c;
    while ((c = getopt_long(argc, argv, "a:h", lopts, NULL)) != -1) {
        switch (c) {
            case 'a': against = optarg; break;
            case 'h': printf("usage: " PROGRAM " diff <path> <ref> [--against=<branch>]\n"); return 0;
            case 0: break;
            default: die("diff: bad flag"); return 1;
        }
    }
    if (optind + 1 >= argc) { die("diff: need a path and a ref"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("diff", argv[optind], rc); fkx_close(x); return 1; }

    fkx_diff_result r;
    memset(&r, 0, sizeof r);
    rc = fkx_diff(x, argv[optind + 1], against, &r);
    if (rc != FKX_OK) { die_err("diff", x, rc); fkx_close(x); return 1; }

    if (json_output) {
        printf("{\"schema_differs\":%s,\"nchangeset\":%d,\"tables\":[",
               r.schema_differs ? "true" : "false", r.nchangeset);
        for (size_t i = 0; i < r.ntables; i++) {
            if (i > 0) fputc(',', stdout);
            fkx_table_diff *t = &r.tables[i];
            const char *st = t->status == FKX_TABLE_COMPARED ? "compared" :
                             t->status == FKX_TABLE_NO_PK ? "no_pk" :
                             t->status == FKX_TABLE_ONLY_HERE ? "only_here" :
                             t->status == FKX_TABLE_ONLY_THERE ? "only_there" :
                             t->status == FKX_TABLE_SCHEMA ? "schema" :
                             t->status == FKX_TABLE_VIRTUAL ? "virtual" : "?";
            printf("{\"name\":");
            json_str(stdout, t->name);
            printf(",\"status\":\"%s\"", st);
            if (t->status == FKX_TABLE_COMPARED)
                printf(",\"inserts\":%" PRIu64 ",\"updates\":%" PRIu64 ",\"deletes\":%" PRIu64,
                       t->inserts, t->updates, t->deletes);
            if (t->status == FKX_TABLE_NO_PK)
                printf(",\"rows_differ\":%s", t->rows_differ ? "true" : "false");
            fputc('}', stdout);
        }
        fputs("]}\n", stdout);
    } else {
        printf("%-24s  %-12s  %7s  %7s  %7s  %s\n",
               "TABLE", "STATUS", "INSERTS", "UPDATES", "DELETES", "ROWS_DIFFER");
        for (size_t i = 0; i < r.ntables; i++) {
            fkx_table_diff *t = &r.tables[i];
            const char *st = t->status == FKX_TABLE_COMPARED ? "compared" :
                             t->status == FKX_TABLE_NO_PK ? "no_pk" :
                             t->status == FKX_TABLE_ONLY_HERE ? "only_here" :
                             t->status == FKX_TABLE_ONLY_THERE ? "only_there" :
                             t->status == FKX_TABLE_SCHEMA ? "schema" :
                             t->status == FKX_TABLE_VIRTUAL ? "virtual" : "?";
            printf("%-24s  %-12s", t->name, st);
            if (t->status == FKX_TABLE_COMPARED)
                printf("  %7" PRIu64 "  %7" PRIu64 "  %7" PRIu64, t->inserts, t->updates, t->deletes);
            else if (t->status == FKX_TABLE_NO_PK)
                printf("  %7s  %7s  %7s  %s", "", "", "", t->rows_differ ? "yes" : "no");
            else
                printf("  %7s  %7s  %7s", "", "", "");
            fputc('\n', stdout);
        }
        if (r.schema_differs) printf("schema_differs: yes\n");
        printf("changeset: %d bytes\n", r.nchangeset);
    }

    fkx_diff_result_free(&r);
    fkx_close(x);
    return 0;
}

static int cmd_promote(int argc, char **argv) {
    static struct option lopts[] = {
        {"into", required_argument, 0, 'i'},
        {"dry-run", no_argument, 0, 'n'},
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    const char *into = NULL;
    int dry_run = 0;
    int c;
    while ((c = getopt_long(argc, argv, "i:nh", lopts, NULL)) != -1) {
        switch (c) {
            case 'i': into = optarg; break;
            case 'n': dry_run = 1; break;
            case 'h': printf("usage: " PROGRAM " promote <path> <name> [--into=<target>] [--dry-run]\n"); return 0;
            case 0: break;
            default: die("promote: bad flag"); return 1;
        }
    }
    if (optind + 1 >= argc) { die("promote: need a path and a name"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("promote", argv[optind], rc); fkx_close(x); return 1; }

    fkx_promote_opts o;
    o.into = into;
    o.dry_run = dry_run;
    fkx_promote_result r;
    memset(&r, 0, sizeof r);
    rc = fkx_promote(x, argv[optind + 1], &o, &r);

    ssize_t nconflicts = (ssize_t)r.nconflicts;
    int ret = 0;
    if (r.refusal && rc == FKX_EREFUSED) {
        if (json_output) {
            printf("{\"method\":\"none\",\"refusal\":");
            json_str(stdout, r.refusal);
            fputs("}\n", stdout);
        } else {
            printf("refused: %s\n", r.refusal);
        }
    } else if (rc == FKX_ECONFLICT || (rc == FKX_OK && r.method == FKX_METHOD_REPLAY && nconflicts > 0)) {
        print_promote_result(&r, nconflicts);
        if (rc != FKX_OK) ret = 1;
    } else if (rc != FKX_OK) {
        die_err("promote", x, rc);
        ret = 1;
    } else {
        print_promote_result(&r, nconflicts);
    }

    fkx_promote_result_free(&r);
    fkx_close(x);
    return ret;
}

static int cmd_undo(int argc, char **argv) {
    static struct option lopts[] = {
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", lopts, NULL)) != -1) {
        switch (c) {
            case 'h': printf("usage: " PROGRAM " undo <path> <name>\n"); return 0;
            case 0: break;
            default: die("undo: bad flag"); return 1;
        }
    }
    if (optind + 1 >= argc) { die("undo: need a path and a name"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("undo", argv[optind], rc); fkx_close(x); return 1; }

    fkx_promote_result r;
    memset(&r, 0, sizeof r);
    rc = fkx_undo(x, argv[optind + 1], &r);

    int ret = 0;
    if (rc == FKX_OK) {
        const char *method = r.method == FKX_METHOD_FAST_FORWARD ? "exact" :
                             r.method == FKX_METHOD_REPLAY ? "replay" : "?";
        if (json_output)
            printf("{\"undone\":true,\"method\":\"%s\"}\n", method);
        else
            printf("undone (%s)\n", method);
    } else if (r.refusal && rc == FKX_EREFUSED) {
        if (json_output) {
            printf("{\"undone\":false,\"refusal\":");
            json_str(stdout, r.refusal);
            fputs("}\n", stdout);
        } else {
            printf("refused: %s\n", r.refusal);
        }
    } else if (rc == FKX_ECONFLICT) {
        ssize_t nconflicts = (ssize_t)r.nconflicts;
        print_promote_result(&r, nconflicts);
    } else {
        die_err("undo", x, rc);
        ret = 1;
    }

    fkx_promote_result_free(&r);
    fkx_close(x);
    return ret;
}

static int cmd_discard(int argc, char **argv) {
    static struct option lopts[] = {
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", lopts, NULL)) != -1) {
        switch (c) {
            case 'h': printf("usage: " PROGRAM " discard <path> <name>\n"); return 0;
            case 0: break;
            default: die("discard: bad flag"); return 1;
        }
    }
    if (optind + 1 >= argc) { die("discard: need a path and a name"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("discard", argv[optind], rc); fkx_close(x); return 1; }

    rc = fkx_discard(x, argv[optind + 1]);
    if (rc != FKX_OK) { die_err("discard", x, rc); fkx_close(x); return 1; }

    if (json_output) printf("{\"discarded\":");
    else               printf("discarded ");
    json_str(stdout, argv[optind + 1]);
    if (json_output) fputs("}\n", stdout);
    else fputc('\n', stdout);

    fkx_close(x);
    return 0;
}

static int cmd_reset(int argc, char **argv) {
    static struct option lopts[] = {
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", lopts, NULL)) != -1) {
        switch (c) {
            case 'h': printf("usage: " PROGRAM " reset <path> <name> <to>\n"); return 0;
            case 0: break;
            default: die("reset: bad flag"); return 1;
        }
    }
    if (optind + 2 >= argc) { die("reset: need a path, a name, and a to"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("reset", argv[optind], rc); fkx_close(x); return 1; }

    rc = fkx_reset(x, argv[optind + 1], argv[optind + 2]);
    if (rc != FKX_OK) { die_err("reset", x, rc); fkx_close(x); return 1; }

    if (json_output) {
        printf("{\"reset\":{\"name\":");
        json_str(stdout, argv[optind + 1]);
        printf(",\"to\":");
        json_str(stdout, argv[optind + 2]);
        printf("}}\n");
    }
    else printf("reset \"%s\" to \"%s\"\n", argv[optind + 1], argv[optind + 2]);

    fkx_close(x);
    return 0;
}

static int cmd_gc(int argc, char **argv) {
    static struct option lopts[] = {
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", lopts, NULL)) != -1) {
        switch (c) {
            case 'h': printf("usage: " PROGRAM " gc <path>\n"); return 0;
            case 0: break;
            default: die("gc: bad flag"); return 1;
        }
    }
    if (optind >= argc) { die("gc: need a path"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("gc", argv[optind], rc); fkx_close(x); return 1; }

    fkx_gc_report rep;
    rc = fkx_gc(x, &rep);
    if (rc != FKX_OK) { die_err("gc", x, rc); fkx_close(x); return 1; }

    uint32_t total = rep.orphan_bases + rep.orphan_undo + rep.unlisted_undo + rep.over_retention;
    if (json_output) {
        printf("{\"orphan_bases\":%u,\"orphan_undo\":%u,\"unlisted_undo\":%u,\"over_retention\":%u,\"total\":%u}\n",
               rep.orphan_bases, rep.orphan_undo, rep.unlisted_undo, rep.over_retention, total);
    } else {
        printf("gc: %u discarded (%u orphan bases, %u orphan undo, %u unlisted undo, %u over retention)\n",
               total, rep.orphan_bases, rep.orphan_undo, rep.unlisted_undo, rep.over_retention);
    }

    fkx_close(x);
    return 0;
}

static int cmd_compact(int argc, char **argv) {
    static struct option lopts[] = {
        {"min-bytes", required_argument, 0, 'm'},
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    uint64_t min_bytes = 0;
    int c;
    while ((c = getopt_long(argc, argv, "m:h", lopts, NULL)) != -1) {
        switch (c) {
            case 'm': min_bytes = (uint64_t)strtoull(optarg, NULL, 10); break;
            case 'h': printf("usage: " PROGRAM " compact <path> [--min-bytes=N]\n"); return 0;
            case 0: break;
            default: die("compact: bad flag"); return 1;
        }
    }
    if (optind >= argc) { die("compact: need a path"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("compact", argv[optind], rc); fkx_close(x); return 1; }

    int did = 0;
    rc = fkx_compact_if_needed(x, min_bytes, &did);
    if (rc != FKX_OK) { die_err("compact", x, rc); fkx_close(x); return 1; }

    if (json_output) printf("{\"compacted\":%s}\n", did ? "true" : "false");
    else printf("compacted: %s\n", did ? "yes" : "no");

    fkx_close(x);
    return 0;
}

struct sql_out {
    int rows_in_set;
    int ncol;
    char **names;
};

static void sql_out_clear(struct sql_out *o) {
    if (o->names) {
        for (int i = 0; i < o->ncol; i++) free(o->names[i]);
        free(o->names);
    }
    o->names = NULL;
    o->ncol = 0;
}

static int sql_same_cols(const struct sql_out *o, int ncol, char **names) {
    if (!o->names || o->ncol != ncol) return 0;
    for (int i = 0; i < ncol; i++) {
        const char *a = o->names[i] ? o->names[i] : "";
        const char *b = (names && names[i]) ? names[i] : "";
        if (strcmp(a, b) != 0) return 0;
    }
    return 1;
}

static int sql_save_cols(struct sql_out *o, int ncol, char **names) {
    char **copy = calloc((size_t)ncol, sizeof *copy);
    if (!copy) return 1;
    for (int i = 0; i < ncol; i++) {
        const char *n = (names && names[i]) ? names[i] : "";
        copy[i] = strdup(n);
        if (!copy[i]) {
            for (int j = 0; j < i; j++) free(copy[j]);
            free(copy);
            return 1;
        }
    }
    sql_out_clear(o);
    o->names = copy;
    o->ncol = ncol;
    o->rows_in_set = 0;
    return 0;
}

static int sql_print_row(void *ud, int ncol, char **vals, char **names) {
    struct sql_out *o = ud;
    int new_set = !sql_same_cols(o, ncol, names);
    int had_rows = o->rows_in_set;
    if (new_set && sql_save_cols(o, ncol, names)) return 1;

    if (json_output) {
        if (new_set && had_rows) fputs("]\n", stdout);
        fputc(o->rows_in_set ? ',' : '[', stdout);
        fputc('{', stdout);
        for (int i = 0; i < ncol; i++) {
            if (i) fputc(',', stdout);
            json_str(stdout, (names && names[i]) ? names[i] : "");
            fputc(':', stdout);
            if (vals && vals[i]) json_str(stdout, vals[i]);
            else fputs("null", stdout);
        }
        fputc('}', stdout);
    } else if (new_set) {
        if (had_rows) fputc('\n', stdout);
        for (int i = 0; i < ncol; i++) {
            if (i) fputc('\t', stdout);
            fputs((names && names[i]) ? names[i] : "", stdout);
        }
        fputc('\n', stdout);
    }
    if (!json_output) {
        for (int i = 0; i < ncol; i++) {
            if (i) fputc('\t', stdout);
            fputs((vals && vals[i]) ? vals[i] : "NULL", stdout);
        }
        fputc('\n', stdout);
    }
    o->rows_in_set++;
    return 0;
}

static int sql_exec(sqlite3 *db, const char *sql) {
    struct sql_out o;
    memset(&o, 0, sizeof o);
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, sql_print_row, &o, &err);
    if (json_output && o.rows_in_set) fputs("]\n", stdout);
    sql_out_clear(&o);
    if (rc != SQLITE_OK) {
        fprintf(stderr, PROGRAM ": sql: %s\n", err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        return 1;
    }
    return 0;
}

static int cmd_sql(int argc, char **argv) {
    static struct option lopts[] = {
        {"write", no_argument, 0, 'w'},
        {"json", no_argument, &json_output, 1},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    int write = 0;
    int c;
    while ((c = getopt_long(argc, argv, "wh", lopts, NULL)) != -1) {
        switch (c) {
            case 'w': write = 1; break;
            case 'h': printf("usage: " PROGRAM " sql <path> <ref> [<sql>] [--write]\n"); return 0;
            case 0: break;
            default: die("sql: bad flag"); return 1;
        }
    }
    if (optind + 1 >= argc) { die("sql: need a path and a ref"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("sql", argv[optind], rc); fkx_close(x); return 1; }

    sqlite3 *db = NULL;
    int flags = write ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) : SQLITE_OPEN_READONLY;
    rc = fkx_open_db(x, argv[optind + 1], flags, &db);
    if (rc != FKX_OK) { die_err("sql", x, rc); fkx_close(x); return 1; }

    int ret = 0;
    const char *sql = optind + 2 < argc ? argv[optind + 2] : NULL;

    if (sql) {
        ret = sql_exec(db, sql);
    } else {
        char buf[4096];
        while (fgets(buf, (int)sizeof buf, stdin)) {
            if (sql_exec(db, buf)) ret = 1;
        }
    }

    sqlite3_close(db);
    fkx_close(x);
    return ret;
}

static int cmd_shell(int argc, char **argv) {
    static struct option lopts[] = {
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", lopts, NULL)) != -1) {
        switch (c) {
            case 'h': printf("usage: " PROGRAM " shell <path>\n"); return 0;
            default: die("shell: bad flag"); return 1;
        }
    }
    if (optind >= argc) { die("shell: need a path"); return 1; }

    fkx *x = NULL;
    int rc = open_store(argv[optind], 0, &x);
    if (rc != FKX_OK) { open_failed("shell", argv[optind], rc); fkx_close(x); return 1; }

    sqlite3 *db = NULL;
    rc = fkx_open_db(x, "main", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, &db);
    if (rc != FKX_OK) { die_err("shell", x, rc); fkx_close(x); return 1; }

    int shell_argc = 1;
    char *shell_argv[] = {PROGRAM, NULL};
    extern int shell_main(int, char **);
    rc = shell_main(shell_argc, shell_argv);

    sqlite3_close(db);
    fkx_close(x);
    return rc;
}

/* ---- entry point ---- */

int main(int argc, char **argv) {
    if (argc < 2) { print_help(stderr); return 1; }

    /* parse global --json/--help before the subcommand */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) { json_output = 1; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_help(stdout); return 0;
        }
    }

    /* find the subcommand (skip --json) */
    const char *cmd = NULL;
    int cmd_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) continue;
        cmd = argv[i]; cmd_idx = i; break;
    }
    if (!cmd) { print_help(stderr); return 1; }

    for (const subcmd *sc = commands; sc->name; sc++) {
        if (!strcmp(cmd, sc->name)) {
            /* shift argv to start at the subcommand name */
            int rc = sc->fn(argc - cmd_idx, argv + cmd_idx);
            if (rc < 0) { /* usage error */ print_help(stderr); return 1; }
            return rc;
        }
    }
    fprintf(stderr, PROGRAM ": unknown command '%s'\n", cmd);
    print_help(stderr);
    return 1;
}
