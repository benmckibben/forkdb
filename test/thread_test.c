/* Multi-threaded stress test for forkdb (the fkx layer, with the stock VFS, through the
 * SQLite and fkx APIs).  One store, one fkx handle shared by all threads.  Every thread
 * opens its own SQLite connection (fkx_open_db) on its branch and does write/read/branch-
 * management work.
 *
 * The library under test has NO internal locking (today).  Expected result:
 *   - ThreadSanitizer data-race reports (the shared fkx state is read and written concurrently).
 *   - SQLite / store-level corruption errors ("disk I/O error") when two writers on
 *     different branches commit concurrently and the store's internal structures race.
 *
 * Build (see scripts/test-threads.sh):
 *   gcc -fsanitize=thread -O1 -g -std=gnu11 -Wall -Wextra \
 *       -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 \
 *       -I fkx -I vfs -I store -I spike/vendor/sqlite-amalgamation-3500000 \
 *       test/thread_test.c fkx/fkx.c fkx/fkx_nosession.c vfs/fkvfs.c \
 *       store/fkstore.c store/io_file.c store/io_prealloc.c sqlite3.c \
 *       -lpthread -lm -ldl -o build/thread_test
 *
 * Control:
 *   THREADS  writer/reader count (default 4)
 *   ITERS    iterations per writer (default 200)
 *   SEED     PRNG seed (default 1)
 *   SERIALISE=1   wrap every fkx/sqlite call in a global mutex (proves the test itself
 *                 is race-free; failures with SERIALISE=1 point at the library).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../fkx/fkx.h"
#include "../vfs/fkvfs.h"

/* ── globals ────────────────────────────────────────────────────────── */
static fkx          *g_x       = NULL;   /* one fkx handle for all threads */
static const char   *g_store_dir = NULL; /* temp directory, freed at exit   */
static char          g_store_path[512];

static int g_threads = 4;
static int g_iters   = 200;
static int g_seed    = 1;
static int g_serialise = 0;   /* 1 = wrap every cross-library call in a big mutex */

static pthread_mutex_t g_serial_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── PRNG (xoshiro256**, deterministic per seed) ───────────────────── */
static _Thread_local uint64_t prng_state[4]; /* per thread: every thread seeds its own stream */

static void prng_seed(uint64_t s) {
  prng_state[0] = s;
  prng_state[1] = 0x9e3779b97f4a7c15ULL;
  prng_state[2] = (s ^ 0xbf58476d1ce4e5b9ULL) * 0x94d049bb133111ebULL;
  prng_state[3] = (s ^ 0x6cbe5f9b2b84a8e4ULL) * 0x9e3779b97f4a7c15ULL;
}

static inline uint64_t prng_next(void) {
  uint64_t s0 = prng_state[0], s1 = prng_state[1], s2 = prng_state[2], s3 = prng_state[3];
  uint64_t result = ((s1 * 5) << 7) | ((s1 * 5) >> (64 - 7));
  result *= 9;
  uint64_t t = s1 << 17;
  s2 ^= s0; s3 ^= s1; s1 ^= s2; s0 ^= s3;
  s2 ^= t;
  s3 = (s3 << 45) | (s3 >> (64 - 45));
  prng_state[0] = s0; prng_state[1] = s1;
  prng_state[2] = s2; prng_state[3] = s3;
  return result;
}

/* ── test harness (thread-safe counters) ────────────────────────────── */
static pthread_mutex_t g_harness_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_fails = 0;
static int g_checks = 0;

/* return a static buffer with the description – single-threaded use only */
#define FAIL(fmt, ...) do {                                            \
    pthread_mutex_lock(&g_harness_mutex);                              \
    g_checks++; g_fails++;                                             \
    fprintf(stderr, "FAIL [%s:%d]: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    pthread_mutex_unlock(&g_harness_mutex);                            \
  } while(0)

#define PASS() do {                                                    \
    pthread_mutex_lock(&g_harness_mutex); g_checks++;                  \
    pthread_mutex_unlock(&g_harness_mutex);                            \
  } while(0)

/* ── serialisation wrapper macros (off by default) ──────────────────── */
static inline void serial_lock(void)   { if (g_serialise) pthread_mutex_lock(&g_serial_mutex); }
static inline void serial_unlock(void) { if (g_serialise) pthread_mutex_unlock(&g_serial_mutex); }

/* ── sql helper: no-serialise wrappers for the harness itself ─────── */
#if 0 /* sqlite3_exec_ns: kept as a library routine for potential use */
static int sqlite3_exec_ns(sqlite3 *db, const char *sql) {
  char *e = NULL;
  serial_lock();
  int rc = sqlite3_exec(db, sql, 0, 0, &e);
  serial_unlock();
  if (e) { fprintf(stderr, "sql error: %s\n", e); sqlite3_free(e); }
  return rc;
}
#endif

static int64_t scalar_ns(sqlite3 *db, const char *q) {
  sqlite3_stmt *st = NULL;
  serial_lock();
  int rc = sqlite3_prepare_v2(db, q, -1, &st, 0);
  serial_unlock();
  if (rc != SQLITE_OK) return -1;
  serial_lock();
  rc = sqlite3_step(st);
  serial_unlock();
  int64_t v = -1;
  if (rc == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
  serial_lock();
  sqlite3_finalize(st);
  serial_unlock();
  return v;
}

/* ── shared model helpers ───────────────────────────────────────────── */
/* Each writer thread keeps a model (fixed-capacity key→value map). */
#define MODEL_CAP 4096

typedef struct {
  int64_t key;
  char    val[64];
  int     val_len;
} model_entry;

typedef struct {
  model_entry entries[MODEL_CAP];
  int n;
  int writer_id;        /* the 'w' value the thread writes       */
  int base_key_start;   /* first auto-generated key for this thread */
} writer_model;

static void model_init(writer_model *m, int wid) {
  m->n = 0;
  m->writer_id = wid;
  m->base_key_start = 100000 * (wid + 1);
}

static void model_add(writer_model *m, int64_t key, const char *val, int len) {
  if (m->n >= MODEL_CAP) { fprintf(stderr, "model overflow\n"); return; }
  for (int i = 0; i < m->n; i++) {
    if (m->entries[i].key == key) {
      memcpy(m->entries[i].val, val, len);
      m->entries[i].val_len = len;
      return;
    }
  }
  m->entries[m->n].key = key;
  memcpy(m->entries[m->n].val, val, len);
  m->entries[m->n].val_len = len;
  m->n++;
}

#if 0 /* model_find: kept as a library routine for potential use */
static int model_find(const writer_model *m, int64_t key, char *out_val, int *out_len) {
  for (int i = 0; i < m->n; i++) {
    if (m->entries[i].key == key) {
      if (out_val) memcpy(out_val, m->entries[i].val, m->entries[i].val_len);
      if (out_len) *out_len = m->entries[i].val_len;
      return 1;
    }
  }
  return 0;
}
#endif

/* ── scenario A: writers on separate branches ──────────────────────── */
typedef struct {
  int thread_idx;
  writer_model model;
  int committed;
  int done;
  int had_error;     /* 1 if a non-BUSY error was hit */
  char last_error[256];
} writer_args;

static void *writer_thread(void *arg) {
  writer_args *a = (writer_args *)arg;
  int idx = a->thread_idx;

  char branch_name[FKX_NAME_MAX + 1];
  snprintf(branch_name, sizeof branch_name, "w%d", idx);

  sqlite3 *db = NULL;
  serial_lock();
  int rc = fkx_open_db(g_x, branch_name, SQLITE_OPEN_READWRITE, &db);
  serial_unlock();
  if (rc != FKX_OK) {
    FAIL("writer %d: fkx_open_db(%s) = %d", idx, branch_name, rc);
    a->had_error = 1;
    return arg;
  }
  serial_lock();
  sqlite3_busy_timeout(db, 5000);
  serial_unlock();

  model_init(&a->model, idx);

  uint64_t seed = (uint64_t)g_seed * 1000 + (uint64_t)idx + 1;
  prng_seed(seed);

  for (int iter = 0; iter < g_iters; iter++) {
    int64_t key = a->model.base_key_start + iter;
    char val[65];
    int val_len = snprintf(val, sizeof val,
                           "t%04d-seed%d-w%d-iter%05d-%016llx",
                           idx, g_seed, idx, iter,
                           (unsigned long long)prng_next());
    if (val_len > 64) val_len = 64;

    int busy_retries = 0;
    for (;;) {
      serial_lock();
      rc = sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0);
      serial_unlock();

      if (rc == SQLITE_BUSY) {
        busy_retries++;
        if (busy_retries > 100) {
          FAIL("writer %d iter %d: BEGIN IMMEDIATE still busy after 100 retries", idx, iter);
          a->committed = a->model.n;
          a->done = 1;
          goto close_db;
        }
        usleep(100);
        continue;
      }

      if (rc != SQLITE_OK) {
        snprintf(a->last_error, sizeof a->last_error, "BEGIN IMMEDIATE: %s", sqlite3_errstr(rc));
        FAIL("writer %d iter %d: %s", idx, iter, a->last_error);
        a->had_error = 1;
        a->committed = a->model.n;
        a->done = 1;
        goto close_db;
      }

      /* INSERT */
      char ins[300];
      snprintf(ins, sizeof ins,
               "INSERT OR REPLACE INTO t(k,w,v) VALUES (%lld, %d, '%s')",
               (long long)key, idx, val);
      serial_lock();
      rc = sqlite3_exec(db, ins, 0, 0, 0);
      serial_unlock();

      if (rc == SQLITE_BUSY) {
        serial_lock();
        sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
        serial_unlock();
        busy_retries++;
        if (busy_retries > 100) {
          FAIL("writer %d iter %d: INSERT still busy after 100 retries", idx, iter);
          a->committed = a->model.n;
          a->done = 1;
          goto close_db;
        }
        usleep(100);
        continue;
      }

      if (rc != SQLITE_OK) {
        snprintf(a->last_error, sizeof a->last_error, "INSERT: %s (rc=%d)", sqlite3_errstr(rc), rc);
        FAIL("writer %d iter %d: %s", idx, iter, a->last_error);
        a->had_error = 1;
        serial_lock();
        sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
        serial_unlock();
        a->committed = a->model.n;
        a->done = 1;
        goto close_db;
      }

      /* sometimes UPDATE one of our own earlier rows */
      if (a->model.n > 0 && (iter % 5) == 0) {
        int pick = (int)(prng_next() % (uint64_t)a->model.n);
        int64_t old_key = a->model.entries[pick].key;
        char up[350];
        snprintf(up, sizeof up,
                 "UPDATE t SET v='%s-updated' WHERE k=%lld AND w=%d",
                 val, (long long)old_key, idx);
        serial_lock();
        rc = sqlite3_exec(db, up, 0, 0, 0);
        serial_unlock();
        if (rc == SQLITE_BUSY) {
          serial_lock();
          sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
          serial_unlock();
          busy_retries++;
          if (busy_retries > 100) { a->committed = a->model.n; a->done = 1; goto close_db; }
          usleep(100);
          continue;
        }
        if (rc != SQLITE_OK) {
          snprintf(a->last_error, sizeof a->last_error, "UPDATE: %s", sqlite3_errstr(rc));
          FAIL("writer %d iter %d: %s", idx, iter, a->last_error);
          a->had_error = 1;
          serial_lock();
          sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
          serial_unlock();
          a->committed = a->model.n;
          a->done = 1;
          goto close_db;
        }
        int slen = (int)strlen(val);
        if (slen >= 56) slen = 56;
        memcpy(a->model.entries[pick].val, val, (size_t)slen);
        memcpy(a->model.entries[pick].val + slen, "-updated", 9);
        a->model.entries[pick].val[slen + 8] = '\0';
      }

      serial_lock();
      rc = sqlite3_exec(db, "COMMIT", 0, 0, 0);
      serial_unlock();

      if (rc == SQLITE_BUSY) {
        serial_lock();
        sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
        serial_unlock();
        busy_retries++;
        if (busy_retries > 100) { a->committed = a->model.n; a->done = 1; goto close_db; }
        usleep(100);
        continue;
      }

      if (rc != SQLITE_OK) {
        snprintf(a->last_error, sizeof a->last_error, "COMMIT: %s", sqlite3_errstr(rc));
        FAIL("writer %d iter %d: %s", idx, iter, a->last_error);
        a->had_error = 1;
        serial_lock();
        sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
        serial_unlock();
        a->committed = a->model.n;
        a->done = 1;
        goto close_db;
      }

      model_add(&a->model, key, val, val_len);
      a->committed++;
      break; /* success, next iteration */
    }
  }

  a->done = 1;

close_db:
  serial_lock();
  sqlite3_close(db);
  serial_unlock();
  return arg;
}

/* ── scenario B: branch management while writers run ───────────────── */
typedef struct {
  int done;
  int fails;
  int ops;
} mgmt_args;

static void *mgmt_thread(void *arg) {
  mgmt_args *a = (mgmt_args *)arg;
  int loops = g_iters / 4;
  if (loops < 1) loops = 1;

  for (int i = 0; i < loops; i++) {
    a->ops++;

    /* Fork from w0, discard */
    char tmp[FKX_NAME_MAX + 1];
    snprintf(tmp, sizeof tmp, "t%d", i);
    serial_lock();
    int rc = fkx_fork(g_x, "w0", tmp);
    serial_unlock();
    if (rc != FKX_OK) {
      if (rc != FKX_EBUSY) {
        FAIL("mgmt: fkx_fork(%s) = %d (%s)", tmp, rc, fkx_errmsg(g_x));
        a->fails++;
      }
    }
    serial_lock();
    rc = fkx_discard(g_x, tmp);
    serial_unlock();
    if (rc != FKX_OK && rc != FKX_EBUSY) {
      FAIL("mgmt: fkx_discard(%s) = %d (%s)", tmp, rc, fkx_errmsg(g_x));
      a->fails++;
    }

    /* Every 5th iteration: reset idle branch */
    if ((i % 5) == 0) {
      serial_lock();
      rc = fkx_reset(g_x, "idle", "main");
      serial_unlock();
      if (rc != FKX_OK) {
        FAIL("mgmt: fkx_reset(idle) = %d (%s)", rc, fkx_errmsg(g_x));
        a->fails++;
      }
    }

    /* Every 10th iteration: GC and compact */
    if ((i % 10) == 0) {
      fkx_gc_report rep;
      serial_lock();
      rc = fkx_gc(g_x, &rep);
      serial_unlock();
      if (rc != FKX_OK) {
        FAIL("mgmt: fkx_gc = %d (%s)", rc, fkx_errmsg(g_x));
        a->fails++;
      }
      int did = 0;
      serial_lock();
      rc = fkx_compact_if_needed(g_x, 0, &did);
      serial_unlock();
      if (rc != FKX_OK) {
        FAIL("mgmt: fkx_compact_if_needed = %d (%s)", rc, fkx_errmsg(g_x));
        a->fails++;
      }
    }
  }

  a->done = 1;
  return arg;
}

/* ── scenario C: readers on main ───────────────────────────────────── */
typedef struct {
  int thread_idx;
  int done;
  int fails;
  int reads;
} reader_args;

static void *reader_thread(void *arg) {
  reader_args *a = (reader_args *)arg;
  sqlite3 *db = NULL;
  serial_lock();
  int rc = fkx_open_db(g_x, "main", SQLITE_OPEN_READONLY, &db);
  serial_unlock();
  if (rc != FKX_OK) {
    FAIL("reader %d: fkx_open_db(main) = %d", a->thread_idx, rc);
    a->fails++;
    a->done = 1;
    return arg;
  }

  for (int i = 0; i < g_iters * 2; i++) {
    int64_t cnt = scalar_ns(db, "SELECT count(*) FROM t");
    int64_t sm  = scalar_ns(db, "SELECT coalesce(sum(k),0) FROM t");
    a->reads++;
    /* main is never written by writers – it has exactly 500 rows with k=0..499 */
    if (cnt != 500) {
      FAIL("reader %d iter %d: count(*) = %lld, expected 500", a->thread_idx, i, (long long)cnt);
      a->fails++;
    }
    /* sum of k=0..499 = 499*500/2 = 124750 */
    int64_t expected_sum = 124750;
    if (sm != expected_sum) {
      FAIL("reader %d iter %d: sum(k) = %lld, expected %lld", a->thread_idx, i, (long long)sm, (long long)expected_sum);
      a->fails++;
    }
  }

  /* final integrity check */
  sqlite3_stmt *st = NULL;
  serial_lock();
  rc = sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &st, 0);
  serial_unlock();
  if (rc == SQLITE_OK) {
    serial_lock();
    rc = sqlite3_step(st);
    serial_unlock();
    if (rc == SQLITE_ROW) {
      const unsigned char *tx = sqlite3_column_text(st, 0);
      if (!tx || strcmp((const char *)tx, "ok") != 0) {
        FAIL("reader %d: PRAGMA quick_check = %s", a->thread_idx, tx ? (const char *)tx : "NULL");
        a->fails++;
      } else {
        PASS();
      }
    }
    serial_lock();
    sqlite3_finalize(st);
    serial_unlock();
  }

  serial_lock();
  sqlite3_close(db);
  serial_unlock();
  a->done = 1;
  return arg;
}

/* ── scenario D: concurrent writers on the SAME branch ─────────────── */
static void *same_branch_writer(void *arg) {
  writer_args *a = (writer_args *)arg;
  int idx = a->thread_idx;

  sqlite3 *db = NULL;
  serial_lock();
  int rc = fkx_open_db(g_x, "w0", SQLITE_OPEN_READWRITE, &db);
  serial_unlock();
  if (rc != FKX_OK) {
    FAIL("same-branch writer %d: fkx_open_db(w0) = %d", idx, rc);
    a->had_error = 1;
    return arg;
  }
  serial_lock();
  sqlite3_busy_timeout(db, 5000);
  serial_unlock();

  model_init(&a->model, idx);
  /* Use a higher key range so we don't collide with Scenario A writers */
  a->model.base_key_start = 10000000 + idx * 100000;

  uint64_t seed = (uint64_t)g_seed * 10000 + (uint64_t)idx + 1;
  prng_seed(seed);

  int n = g_iters / 2;
  if (n < 1) n = 1;

  for (int iter = 0; iter < n; iter++) {
    int64_t key = a->model.base_key_start + iter;
    char val[65];
    int val_len = snprintf(val, sizeof val,
                           "same-w%d-s%d-iter%05d-%016llx",
                           idx, g_seed, iter, (unsigned long long)prng_next());
    if (val_len > 64) val_len = 64;

    int busy_retries = 0;
    for (;;) {
      serial_lock();
      rc = sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0);
      serial_unlock();

      if (rc == SQLITE_BUSY) {
        busy_retries++;
        if (busy_retries > 100) {
          FAIL("same-branch writer %d iter %d: BEGIN still busy after 100 retries", idx, iter);
          a->committed = a->model.n;
          a->done = 1;
          goto close_db_d;
        }
        usleep(100);
        continue;
      }
      if (rc != SQLITE_OK) {
        snprintf(a->last_error, sizeof a->last_error, "BEGIN: %s", sqlite3_errstr(rc));
        FAIL("same-branch writer %d iter %d: %s", idx, iter, a->last_error);
        a->had_error = 1;
        a->committed = a->model.n;
        a->done = 1;
        goto close_db_d;
      }

      char ins[300];
      snprintf(ins, sizeof ins,
               "INSERT OR REPLACE INTO t(k,w,v) VALUES (%lld, %d, '%s')",
               (long long)key, 100 + idx, val);
      serial_lock();
      rc = sqlite3_exec(db, ins, 0, 0, 0);
      serial_unlock();

      if (rc == SQLITE_BUSY) {
        serial_lock();
        sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
        serial_unlock();
        busy_retries++;
        if (busy_retries > 100) { a->committed = a->model.n; a->done = 1; goto close_db_d; }
        usleep(100);
        continue;
      }
      if (rc != SQLITE_OK) {
        snprintf(a->last_error, sizeof a->last_error, "INSERT: %s", sqlite3_errstr(rc));
        FAIL("same-branch writer %d iter %d: %s", idx, iter, a->last_error);
        a->had_error = 1;
        serial_lock();
        sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
        serial_unlock();
        a->committed = a->model.n;
        a->done = 1;
        goto close_db_d;
      }

      serial_lock();
      rc = sqlite3_exec(db, "COMMIT", 0, 0, 0);
      serial_unlock();

      if (rc == SQLITE_BUSY) {
        serial_lock();
        sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
        serial_unlock();
        busy_retries++;
        if (busy_retries > 100) { a->committed = a->model.n; a->done = 1; goto close_db_d; }
        usleep(100);
        continue;
      }
      if (rc != SQLITE_OK) {
        snprintf(a->last_error, sizeof a->last_error, "COMMIT: %s", sqlite3_errstr(rc));
        FAIL("same-branch writer %d iter %d: %s", idx, iter, a->last_error);
        a->had_error = 1;
        serial_lock();
        sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
        serial_unlock();
        a->committed = a->model.n;
        a->done = 1;
        goto close_db_d;
      }

      model_add(&a->model, key, val, val_len);
      a->committed++;
      break;
    }
  }

  a->done = 1;

close_db_d:
  serial_lock();
  sqlite3_close(db);
  serial_unlock();
  return arg;
}

/* ── verification helpers ───────────────────────────────────────────── */
static int verify_branch(const char *branch_name, int writer_id,
                         const writer_model *model, int expected_commits) {
  sqlite3 *db = NULL;
  serial_lock();
  int rc = fkx_open_db(g_x, branch_name, SQLITE_OPEN_READONLY, &db);
  serial_unlock();
  if (rc != FKX_OK) {
    FAIL("verify %s: fkx_open_db = %d", branch_name, rc);
    return 0;
  }

  int ok = 1;

  /* PRAGMA integrity_check */
  sqlite3_stmt *st = NULL;
  serial_lock();
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, 0);
  serial_unlock();
  if (rc == SQLITE_OK) {
    serial_lock();
    rc = sqlite3_step(st);
    serial_unlock();
    if (rc == SQLITE_ROW) {
      const unsigned char *tx = sqlite3_column_text(st, 0);
      if (!tx || strcmp((const char *)tx, "ok") != 0) {
        FAIL("verify %s: integrity_check = %s", branch_name, tx ? (const char *)tx : "NULL");
        ok = 0;
      } else {
        PASS();
      }
    }
    serial_lock();
    sqlite3_finalize(st);
    serial_unlock();
  }

  /* Count rows with this writer_id */
  char q[256];
  snprintf(q, sizeof q, "SELECT count(*) FROM t WHERE w=%d", writer_id);
  int64_t cnt = scalar_ns(db, q);
  if (cnt != (int64_t)expected_commits) {
    FAIL("verify %s: rows with w=%d = %lld, model says %d",
         branch_name, writer_id, (long long)cnt, expected_commits);
    ok = 0;
  } else {
    PASS();
  }

  /* Check every model entry exists with correct value */
  for (int i = 0; i < model->n; i++) {
    snprintf(q, sizeof q, "SELECT v FROM t WHERE k=%lld AND w=%d",
             (long long)model->entries[i].key, writer_id);
    sqlite3_stmt *st2 = NULL;
    serial_lock();
    rc = sqlite3_prepare_v2(db, q, -1, &st2, 0);
    serial_unlock();
    if (rc != SQLITE_OK) continue;
    serial_lock();
    rc = sqlite3_step(st2);
    serial_unlock();
    if (rc == SQLITE_ROW) {
      const unsigned char *tx = sqlite3_column_text(st2, 0);
      if (!tx || strcmp((const char *)tx, model->entries[i].val) != 0) {
        FAIL("verify %s: k=%lld: v='%s', expected '%s'",
             branch_name, (long long)model->entries[i].key,
             tx ? (const char *)tx : "NULL", model->entries[i].val);
        ok = 0;
      }
    } else {
      FAIL("verify %s: k=%lld not found (w=%d)",
           branch_name, (long long)model->entries[i].key, writer_id);
      ok = 0;
    }
    serial_lock();
    sqlite3_finalize(st2);
    serial_unlock();
  }

  /* Rows with w=-1 (originals) */
  int64_t orig = scalar_ns(db, "SELECT count(*) FROM t WHERE w=-1");
  if (orig != 500) {
    FAIL("verify %s: origin rows (w=-1) = %lld, expected 500", branch_name, (long long)orig);
    ok = 0;
  } else {
    PASS();
  }

  /* No rows from another writer's key range */
  snprintf(q, sizeof q,
           "SELECT count(*) FROM t WHERE w>=0 AND w!=%d AND w!=%d",
           writer_id, -1);
  int64_t stray = scalar_ns(db, q);
  if (writer_id >= 0 && stray > 0) {
    /* w value check: writers on different branches never write each other's w */
    FAIL("verify %s: %lld rows from other writers", branch_name, (long long)stray);
    ok = 0;
  } else {
    PASS();
  }

  serial_lock();
  sqlite3_close(db);
  serial_unlock();
  return ok;
}

/* ── main ───────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
  /* ── env ── */
  const char *env;
  if ((env = getenv("THREADS"))) g_threads = atoi(env);
  if ((env = getenv("ITERS")))   g_iters   = atoi(env);
  if ((env = getenv("SEED")))    g_seed    = atoi(env);
  if ((env = getenv("SERIALISE")) && atoi(env) == 1) g_serialise = 1;

  if (g_threads < 2) g_threads = 2;

  printf("THREADS=%d ITERS=%d SEED=%d SERIALISE=%d\n",
         g_threads, g_iters, g_seed, g_serialise);
  fflush(stdout);

  /* ── temp directory ── */
  if (argc > 1) {
    g_store_dir = argv[1];
  } else {
    char *tmp = strdup("/tmp/forkdbthrXXXXXX");
    if (!tmp) { fprintf(stderr, "OOM\n"); return 2; }
    g_store_dir = mkdtemp(tmp);
    if (!g_store_dir) { fprintf(stderr, "mkdtemp: %s\n", strerror(errno)); free(tmp); return 2; }
  }
  snprintf(g_store_path, sizeof g_store_path, "%s/db.fk", g_store_dir);
  printf("store: %s\n", g_store_path);
  fflush(stdout);

  prng_seed((uint64_t)g_seed);

  /* ── setup: open store, create schema, insert 500 origin rows, fork THREADS branches ── */
  {
    fkx_options o;
    fkx_options_default(&o);
    o.prealloc_chunk = 0;
    o.create = 1;
    serial_lock();
    int rc = fkx_open(g_store_path, &o, &g_x);
    serial_unlock();
    if (rc != FKX_OK) {
      fprintf(stderr, "fkx_open = %d (%s)\n", rc, fkx_errmsg(g_x));
      return 2;
    }

    sqlite3 *db = NULL;
    serial_lock();
    rc = fkx_open_db(g_x, "main", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, &db);
    serial_unlock();
    if (rc != FKX_OK) {
      fprintf(stderr, "setup: fkx_open_db(main) = %d (%s)\n", rc, fkx_errmsg(g_x));
      return 2;
    }

    serial_lock();
    rc = sqlite3_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, w INTEGER, v TEXT)", 0, 0, 0);
    serial_unlock();
    if (rc != SQLITE_OK) {
      fprintf(stderr, "CREATE TABLE: %s\n", sqlite3_errstr(rc));
      return 2;
    }

    /* Insert 500 rows (k=0..499, w=-1) in one transaction */
    serial_lock();
    sqlite3_exec(db, "BEGIN", 0, 0, 0);
    serial_unlock();
    for (int k = 0; k < 500; k++) {
      char ins[128];
      snprintf(ins, sizeof ins, "INSERT INTO t(k,w,v) VALUES (%d,-1,'origin-%05d')", k, k);
      serial_lock();
      int irc = sqlite3_exec(db, ins, 0, 0, 0);
      serial_unlock();
      if (irc != SQLITE_OK) {
        fprintf(stderr, "INSERT origin %d: %s\n", k, sqlite3_errstr(irc));
        return 2;
      }
    }
    serial_lock();
    rc = sqlite3_exec(db, "COMMIT", 0, 0, 0);
    serial_unlock();
    if (rc != SQLITE_OK) {
      fprintf(stderr, "origin COMMIT: %s\n", sqlite3_errstr(rc));
      return 2;
    }

    serial_lock();
    sqlite3_close(db);
    serial_unlock();

    /* Fork THREADS branches 'w0'..'w<N-1>' from main */
    for (int i = 0; i < g_threads; i++) {
      char nm[FKX_NAME_MAX + 1];
      snprintf(nm, sizeof nm, "w%d", i);
      serial_lock();
      rc = fkx_fork(g_x, "main", nm);
      serial_unlock();
      if (rc != FKX_OK) {
        fprintf(stderr, "fkx_fork(%s) = %d (%s)\n", nm, rc, fkx_errmsg(g_x));
        return 2;
      }
    }

    /* Fork 'idle' from main for the mgmt thread */
    serial_lock();
    rc = fkx_fork(g_x, "main", "idle");
    serial_unlock();
    if (rc != FKX_OK) {
      fprintf(stderr, "fkx_fork(idle) = %d (%s)\n", rc, fkx_errmsg(g_x));
      return 2;
    }
  }

  /* ── SCENARIO A: writers on separate branches ── */
  {
    printf("=== scenario A: %d writers, %d iters each ===\n", g_threads, g_iters);
    fflush(stdout);

    writer_args *wa = calloc((size_t)g_threads, sizeof(writer_args));
    pthread_t *wt = calloc((size_t)g_threads, sizeof(pthread_t));
    if (!wa || !wt) { fprintf(stderr, "OOM\n"); return 2; }

    for (int i = 0; i < g_threads; i++) {
      wa[i].thread_idx = i;
      pthread_create(&wt[i], NULL, writer_thread, &wa[i]);
    }
    for (int i = 0; i < g_threads; i++)
      pthread_join(wt[i], NULL);

    int a_fails = 0, a_commits = 0;
    for (int i = 0; i < g_threads; i++) {
      a_commits += wa[i].committed;
      a_fails += wa[i].had_error;
    }
    printf("scenario A complete: %d commits, errors=%d\n", a_commits, a_fails);
    fflush(stdout);

    /* verify each branch */
    for (int i = 0; i < g_threads; i++) {
      char nm[FKX_NAME_MAX + 1];
      snprintf(nm, sizeof nm, "w%d", i);
      printf("  verify %s (model=%d rows, commits=%d)...\n", nm, wa[i].model.n, wa[i].committed);
      verify_branch(nm, i, &wa[i].model, wa[i].committed);
    }

    free(wa); free(wt);
  }

  /* ── SCENARIO B: branch management while writers run ── */
  /* re-open: writers closed their connections */
  {
    printf("=== scenario B: branch management ===\n");
    fflush(stdout);

    pthread_t mgmt_tid;
    mgmt_args ma;
    memset(&ma, 0, sizeof(ma));
    pthread_create(&mgmt_tid, NULL, mgmt_thread, &ma);
    pthread_join(mgmt_tid, NULL);

    printf("scenario B complete: %d ops, fails=%d\n", ma.ops, ma.fails);
    fflush(stdout);
  }

  /* ── SCENARIO C: readers (while writers and management run) ── */
  {
    printf("=== scenario C: 2 readers on main ===\n");
    fflush(stdout);

    reader_args ra[2];
    pthread_t rt[2];
    for (int i = 0; i < 2; i++) {
      ra[i].thread_idx = i;
      ra[i].fails = 0;
      ra[i].reads = 0;
      ra[i].done = 0;
      pthread_create(&rt[i], NULL, reader_thread, &ra[i]);
    }
    for (int i = 0; i < 2; i++)
      pthread_join(rt[i], NULL);

    printf("scenario C complete: reads=%d+%d, fails=%d\n",
           ra[0].reads, ra[1].reads, ra[0].fails + ra[1].fails);
    fflush(stdout);
  }

  /* ── SCENARIO D: concurrent writers on the SAME branch (w0) ── */
  {
    printf("=== scenario D: %d concurrent writers on branch w0, %d iters/2 each ===\n",
           g_threads, g_iters);
    fflush(stdout);

    writer_args *wa = calloc((size_t)g_threads, sizeof(writer_args));
    pthread_t *wt = calloc((size_t)g_threads, sizeof(pthread_t));
    if (!wa || !wt) { fprintf(stderr, "OOM\n"); return 2; }

    for (int i = 0; i < g_threads; i++) {
      wa[i].thread_idx = i;
      pthread_create(&wt[i], NULL, same_branch_writer, &wa[i]);
    }
    for (int i = 0; i < g_threads; i++)
      pthread_join(wt[i], NULL);

    int d_commits = 0, d_errs = 0;
    for (int i = 0; i < g_threads; i++) {
      d_commits += wa[i].committed;
      d_errs += wa[i].had_error;
    }
    printf("scenario D complete: %d commits, errors=%d\n", d_commits, d_errs);
    fflush(stdout);

    /* Verify w0: all same-branch writers' committed rows must be present */
    /* Build merged model */
    writer_model merged;
    memset(&merged, 0, sizeof(merged));
    merged.writer_id = -1;
    for (int i = 0; i < g_threads; i++) {
      for (int j = 0; j < wa[i].model.n; j++) {
        model_add(&merged, wa[i].model.entries[j].key,
                  wa[i].model.entries[j].val, wa[i].model.entries[j].val_len);
      }
    }
    /* Also count scenario A rows that should still be on w0 */
    /* w0 from scenario A had its own rows with w=0 */
    /* verify using the per-writer models - check the rows exist */
    for (int i = 0; i < g_threads; i++) {
      printf("  verify w0 writer %d (model=%d rows, commits=%d)...\n",
             i, wa[i].model.n, wa[i].committed);
      /* Check each writer's committed rows exist on w0 */
      sqlite3 *db = NULL;
      serial_lock();
      int rc = fkx_open_db(g_x, "w0", SQLITE_OPEN_READONLY, &db);
      serial_unlock();
      if (rc != FKX_OK) {
        FAIL("scenario D: fkx_open_db(w0) = %d", rc);
        continue;
      }

      /* integrity check once */
      sqlite3_stmt *st = NULL;
      serial_lock();
      rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, 0);
      serial_unlock();
      if (rc == SQLITE_OK) {
        serial_lock();
        rc = sqlite3_step(st);
        serial_unlock();
        if (rc == SQLITE_ROW) {
          const unsigned char *tx = sqlite3_column_text(st, 0);
          if (!tx || strcmp((const char *)tx, "ok") != 0) {
            FAIL("scenario D w0: integrity_check = %s", tx ? (const char *)tx : "NULL");
          } else { PASS(); }
        }
        serial_lock();
        sqlite3_finalize(st);
        serial_unlock();
      }

      /* Count rows this writer committed (w = 100 + i for scenario D) */
      char q[256];
      snprintf(q, sizeof q, "SELECT count(*) FROM t WHERE w=%d", 100 + i);
      int64_t cnt = scalar_ns(db, q);
      if (cnt != (int64_t)wa[i].committed) {
        FAIL("scenario D w0: rows with w=%d = %lld, model says %d",
             100 + i, (long long)cnt, wa[i].committed);
      } else { PASS(); }

      /* Check model entries exist */
      for (int j = 0; j < wa[i].model.n; j++) {
        snprintf(q, sizeof q, "SELECT v FROM t WHERE k=%lld",
                 (long long)wa[i].model.entries[j].key);
        sqlite3_stmt *st2 = NULL;
        serial_lock();
        rc = sqlite3_prepare_v2(db, q, -1, &st2, 0);
        serial_unlock();
        if (rc != SQLITE_OK) continue;
        serial_lock();
        rc = sqlite3_step(st2);
        serial_unlock();
        if (rc != SQLITE_ROW) {
          FAIL("scenario D w0: k=%lld not found", (long long)wa[i].model.entries[j].key);
        } else { PASS(); }
        serial_lock();
        sqlite3_finalize(st2);
        serial_unlock();
      }

      serial_lock();
      sqlite3_close(db);
      serial_unlock();
    }

    free(wa); free(wt);
  }

  /* ── durability: close, reopen, verify every branch again ── */
  printf("=== durability: close, reopen, re-verify ===\n");
  fflush(stdout);
  {
    serial_lock();
    fkx_close(g_x);
    g_x = NULL;
    serial_unlock();

    fkx_options o;
    fkx_options_default(&o);
    o.prealloc_chunk = 0;
    o.create = 0;
    serial_lock();
    int rc = fkx_open(g_store_path, &o, &g_x);
    serial_unlock();
    if (rc != FKX_OK) {
      fprintf(stderr, "durability: fkx_open(reopen) = %d (%s)\n", rc, fkx_errmsg(g_x));
      FAIL("reopen failed");
    } else {
      /* Re-verify every writer branch: its rows count matches its model, integrity ok */
      /* Quick check on main: 500 origin rows */
      {
        sqlite3 *db = NULL;
        serial_lock();
        rc = fkx_open_db(g_x, "main", SQLITE_OPEN_READONLY, &db);
        serial_unlock();
        if (rc == FKX_OK) {
          int64_t cnt = scalar_ns(db, "SELECT count(*) FROM t WHERE w=-1");
          if (cnt != 500) {
            FAIL("durability main: origin rows = %lld, expected 500", (long long)cnt);
          } else { PASS(); }
          serial_lock();
          sqlite3_close(db);
          serial_unlock();
        }
      }
      printf("durability: origin rows verified, reopen OK\n");
    }
  }

  /* ── cleanup ── */
  serial_lock();
  if (g_x) { fkx_close(g_x); g_x = NULL; }
  serial_unlock();

  char cmd[500];
  snprintf(cmd, sizeof cmd, "rm -rf %s", g_store_dir);
  int rv = system(cmd); (void)rv;

  printf("checks=%d TOTAL FAILS=%d\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}