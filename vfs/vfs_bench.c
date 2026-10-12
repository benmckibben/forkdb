/* Benchmark: SQLite on plain files (rollback journal, WAL) against SQLite through fkvfs on the durable store.
 * usage: vfs_bench DIR [REPS]    prints one line per measurement: R <rep> <config> <metric> <value>
 * Every number is from one machine; see docs/VFS-SPIKE.md for how to read them. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fkvfs.h"

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void R(int rep, const char *cfg, const char *metric, double v) { printf("R %d %s %s %.6f\n", rep, cfg, metric, v); }
static void die(const char *m) { fprintf(stderr, "bench: %s\n", m); exit(2); }
static void must(int rc, const char *what) { if (rc != SQLITE_OK) { fprintf(stderr, "bench: %s failed (%d)\n", what, rc); exit(2); } }
static long long scalar(sqlite3 *db, const char *q) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db, q, -1, &st, 0) != SQLITE_OK) die(q);
  long long v = -1;
  if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}
static long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : 0; }

/* an fk_io that counts what the store asks of the disk */
#define PREALLOC_CHUNK (64u << 20)
/* mode bits: what this wrapper does to the store's I/O (an experiment on the commit path; the store itself is unchanged) */
enum { M_PREALLOC = 1, M_COALESCE = 2 };
typedef struct {
  fk_io real;
  uint64_t fsyncs, pwrites, pwrite_bytes, real_pwrites;
  double fsync_s;
  int mode;
  uint64_t prealloc_end;                 /* M_PREALLOC: the file is already written (zeros or data) up to here */
  uint8_t *pend; size_t pend_len, pend_cap; uint64_t pend_off; /* M_COALESCE: contiguous writes not yet handed to the file */
} Cnt;
static int64_t c_size(void *c) { Cnt *k = c; return k->real.size(k->real.ctx); }
static int c_flush(Cnt *k) {
  if (!k->pend_len) return 0;
  size_t n = k->pend_len;
  k->pend_len = 0;
  k->real_pwrites++;
  return k->real.pwrite(k->real.ctx, k->pend_off, k->pend, n);
}
static int c_pread(void *c, uint64_t o, void *b, size_t n) { Cnt *k = c; if (c_flush(k)) return -1; return k->real.pread(k->real.ctx, o, b, n); }
static int c_extend(Cnt *k, uint64_t upto) { /* write zeros so the file is allocated and written (not merely sparse or unwritten) up to upto */
  static uint8_t *z;
  if (!z) z = calloc(1, 1u << 20);
  while (k->prealloc_end < upto) {
    for (uint32_t d = 0; d < PREALLOC_CHUNK; d += 1u << 20)
      if (k->real.pwrite(k->real.ctx, k->prealloc_end + d, z, 1u << 20) != 0) return -1;
    k->prealloc_end += PREALLOC_CHUNK;
    if (k->real.fsync(k->real.ctx) != 0) return -1; /* make the extents and the size durable now, once */
  }
  return 0;
}
static int c_pwrite(void *c, uint64_t o, const void *b, size_t n) {
  Cnt *k = c;
  k->pwrites++;
  k->pwrite_bytes += n;
  if ((k->mode & M_PREALLOC) && c_extend(k, o + n) != 0) return -1;
  if (k->mode & M_COALESCE) {
    if (k->pend_len && o != k->pend_off + k->pend_len && c_flush(k)) return -1;
    if (k->pend_len + n > k->pend_cap) { size_t nc = (k->pend_len + n) * 2 + 4096; uint8_t *p = realloc(k->pend, nc); if (!p) return -1; k->pend = p; k->pend_cap = nc; }
    if (!k->pend_len) k->pend_off = o;
    memcpy(k->pend + k->pend_len, b, n);
    k->pend_len += n;
    return 0;
  }
  k->real_pwrites++;
  return k->real.pwrite(k->real.ctx, o, b, n);
}
static int c_trunc(void *c, uint64_t n) { Cnt *k = c; if (c_flush(k)) return -1; if (k->prealloc_end > n) k->prealloc_end = n; return k->real.truncate(k->real.ctx, n); }
static int c_fsync(void *c) { Cnt *k = c; if (c_flush(k)) return -1; double t = now(); int rc = k->real.fsync(k->real.ctx); k->fsync_s += now() - t; k->fsyncs++; return rc; }
static int c_sc(void *c, fk_io *t) { Cnt *k = c; return k->real.sibling_create(k->real.ctx, t); }
static int c_sr(void *c, fk_io *t) { Cnt *k = c; return k->real.sibling_rename(k->real.ctx, t); }
static int c_sd(void *c) { Cnt *k = c; return k->real.sync_dir(k->real.ctx); }
static void c_rm(void *c, fk_io *t) { Cnt *k = c; k->real.sibling_remove(k->real.ctx, t); }
static fk_io wrap(Cnt *k) {
  fk_io io = k->real;
  io.ctx = k;
  io.size = c_size; io.pread = c_pread; io.pwrite = c_pwrite; io.truncate = c_trunc; io.fsync = c_fsync;
  io.sibling_create = c_sc; io.sibling_rename = c_sr; io.sync_dir = c_sd; io.sibling_remove = c_rm;
  return io;
}

static const char *CFG[] = {"file/DELETE", "file/WAL", "forkdb/batch", "forkdb/journal", "forkdb/prealloc", "forkdb/coalesce", "forkdb/both", "forkdb/pa-1M", "forkdb/pa-4M", "forkdb/pa-16M", "forkdb/pa-64M"};
static const int KMODE[] = {0, 0, 0, 0, M_PREALLOC, M_COALESCE, M_PREALLOC | M_COALESCE, 0, 0, 0, 0};
/* the real preallocation layer (store/io_prealloc.c), on top of the counting io, so the counters include the zero fill; 0 = none */
static const uint64_t KCHUNK[] = {0, 0, 0, 0, 0, 0, 0, 1u << 20, 4u << 20, 16u << 20, 64u << 20};

typedef struct { Cnt c; fk_io io; fk_store *s; FkVfs *v; sqlite3 *db; } Fk;

static void fk_start(Fk *f, const char *path, int batch, int mode, uint64_t chunk) {
  memset(f, 0, sizeof *f);
  f->c.mode = mode;
  if (fk_file_open(path, &f->c.real) != 0) die("fk_file_open");
  f->c.prealloc_end = (uint64_t)f->c.real.size(f->c.real.ctx);
  f->io = wrap(&f->c);
  if (chunk && fk_prealloc_wrap(&f->io, chunk) != FK_OK) die("prealloc wrap");
  if (fk_open(&f->io, &f->s) != 0) die("fk_open");
  must(fkvfs_register(f->s, "fkb", &f->v), "register");
  fkvfs_set_batch_atomic(f->v, batch);
  must(sqlite3_open_v2("b0", &f->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "fkb"), "open b0");
}
static void fk_stop(Fk *f) {
  sqlite3_close(f->db);
  must(fkvfs_unregister(f->v), "unregister");
  fk_close(f->s);
  fk_prealloc_close(&f->io); /* cut the zeros off (a no-op without the layer) */
  fk_file_close(&f->c.real);
}

static double pct(double *a, int n, double p) { return a[(int)((n - 1) * p)]; }

static void fsync_floor(int rep, const char *dir) {
  char p[400];
  snprintf(p, sizeof p, "%s/floor.bin", dir);
  int fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) die("floor open");
  char buf[4096];
  memset(buf, 7, sizeof buf);
  double t[300];
  for (int i = 0; i < 300; i++) {
    double a = now();
    if (pwrite(fd, buf, sizeof buf, (off_t)i * 4096) != 4096) die("floor write");
    if (fsync(fd) != 0) die("floor fsync");
    t[i] = (now() - a) * 1e6;
  }
  close(fd);
  unlink(p);
  qsort(t, 300, sizeof *t, cmpd);
  R(rep, "disk", "write4k+fsync_p50_us", pct(t, 300, 0.5));
  R(rep, "disk", "write4k+fsync_p99_us", pct(t, 300, 0.99));
}

static void run(int rep, int kind, const char *dir) {
  const char *cfg = CFG[kind];
  char path[400], copy[400];
  snprintf(path, sizeof path, "%s/bench-%d.%s", dir, kind, kind < 2 ? "db" : "fk");
  snprintf(copy, sizeof copy, "%s/bench-%d.copy", dir, kind);
  char wal[420], jr[420];
  snprintf(wal, sizeof wal, "%s-wal", path);
  snprintf(jr, sizeof jr, "%s-journal", path);
  unlink(path); unlink(wal); unlink(jr); unlink(copy);
  Fk f;
  sqlite3 *db = NULL;
  if (kind >= 2) { fk_start(&f, path, kind == 2, KMODE[kind], KCHUNK[kind]); db = f.db; }
  else {
    must(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL), "open");
    must(sqlite3_exec(db, kind == 1 ? "PRAGMA journal_mode=WAL" : "PRAGMA journal_mode=DELETE", 0, 0, 0), "journal_mode");
    must(sqlite3_exec(db, "PRAGMA synchronous=FULL", 0, 0, 0), "synchronous");
  }
  must(sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)", 0, 0, 0), "create");
  sqlite3_stmt *ins;
  must(sqlite3_prepare_v2(db, "INSERT INTO t(v) VALUES (randomblob(100))", -1, &ins, 0), "prepare");

  /* W1: single-row autocommit transactions */
  enum { N1 = 2000 };
  static double lat[N1];
  uint64_t fs0 = kind >= 2 ? f.c.fsyncs : 0, pb0 = kind >= 2 ? f.c.pwrite_bytes : 0, rp0 = kind >= 2 ? f.c.real_pwrites : 0;
  double w1 = now();
  for (int i = 0; i < N1; i++) {
    double a = now();
    if (sqlite3_step(ins) != SQLITE_DONE) die("w1 insert");
    sqlite3_reset(ins);
    lat[i] = (now() - a) * 1e6;
  }
  w1 = now() - w1;
  qsort(lat, N1, sizeof *lat, cmpd);
  double sum = 0;
  for (int i = 0; i < N1; i++) sum += lat[i];
  R(rep, cfg, "w1_txn_per_s", N1 / w1);
  R(rep, cfg, "w1_p50_us", pct(lat, N1, 0.5));
  R(rep, cfg, "w1_p99_us", pct(lat, N1, 0.99));
  if (kind >= 2) {
    R(rep, cfg, "w1_fsyncs_per_txn", (double)(f.c.fsyncs - fs0) / N1);
    R(rep, cfg, "w1_bytes_written_per_txn", (double)(f.c.pwrite_bytes - pb0) / N1);
    R(rep, cfg, "w1_pwrites_per_txn", (double)(f.c.real_pwrites - rp0) / N1);
  }

  /* W2: 100 transactions of 1000 rows */
  double t2[100];
  for (int i = 0; i < 100; i++) {
    double a = now();
    must(sqlite3_exec(db, "BEGIN", 0, 0, 0), "begin");
    for (int j = 0; j < 1000; j++) { if (sqlite3_step(ins) != SQLITE_DONE) die("w2 insert"); sqlite3_reset(ins); }
    must(sqlite3_exec(db, "COMMIT", 0, 0, 0), "commit");
    t2[i] = (now() - a) * 1e3;
  }
  qsort(t2, 100, sizeof *t2, cmpd);
  R(rep, cfg, "w2_ms_per_1000_row_txn_p50", pct(t2, 100, 0.5));
  R(rep, cfg, "w2_ms_per_1000_row_txn_p99", pct(t2, 100, 0.99));

  /* W3: one big transaction, 200000 rows (the page cache is the default 2 MB, so this spills) */
  double a = now();
  must(sqlite3_exec(db, "BEGIN", 0, 0, 0), "begin");
  for (int j = 0; j < 200000; j++) { if (sqlite3_step(ins) != SQLITE_DONE) die("w3 insert"); sqlite3_reset(ins); }
  must(sqlite3_exec(db, "COMMIT", 0, 0, 0), "commit");
  R(rep, cfg, "w3_s_200k_rows_one_txn", now() - a);
  sqlite3_finalize(ins);
  if (kind == 1) must(sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", 0, 0, 0), "checkpoint"); /* steady state for the reads below */

  long long rows = scalar(db, "SELECT count(*) FROM t");
  /* W4: random primary-key reads, W5: full scan */
  sqlite3_stmt *get;
  must(sqlite3_prepare_v2(db, "SELECT length(v) FROM t WHERE id=?", -1, &get, 0), "prepare get");
  uint64_t x = 88172645463325252ull;
  a = now();
  for (int i = 0; i < 100000; i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    sqlite3_bind_int64(get, 1, (long long)(x % (uint64_t)rows) + 1);
    if (sqlite3_step(get) != SQLITE_ROW) die("get");
    sqlite3_reset(get);
  }
  R(rep, cfg, "w4_us_per_random_read", (now() - a) / 100000 * 1e6);
  sqlite3_finalize(get);
  a = now();
  (void)scalar(db, "SELECT sum(length(v)) FROM t");
  R(rep, cfg, "w5_ms_full_scan", (now() - a) * 1e3);

  long pages = (long)scalar(db, "PRAGMA page_count");
  R(rep, cfg, "db_pages", (double)pages);

  /* W6: what a "copy of the database" costs */
  if (kind >= 2) {
    uint64_t fb0, lb0, fb1, lb1;
    fk_stats(f.s, &fb0, &lb0);
    double fl[200];
    for (int i = 0; i < 200; i++) {
      uint32_t b = 0;
      double t = now();
      if (fk_fork(f.s, 0, &b) != FK_OK) die("fork");
      fl[i] = (now() - t) * 1e6;
      if (fk_discard(f.s, b) != FK_OK) die("discard");
    }
    qsort(fl, 200, sizeof *fl, cmpd);
    R(rep, cfg, "w6_fork_p50_us", pct(fl, 200, 0.5));
    R(rep, cfg, "w6_fork_p99_us", pct(fl, 200, 0.99));
    uint32_t b1 = 0;
    if (fk_fork(f.s, 0, &b1) != FK_OK) die("fork");
    sqlite3 *fdb = NULL;
    char nm[16];
    fkvfs_branch_name(b1, nm, sizeof nm);
    must(sqlite3_open_v2(nm, &fdb, SQLITE_OPEN_READWRITE, "fkb"), "open fork");
    (void)scalar(fdb, "SELECT count(*) FROM t");
    fk_stats(f.s, &fb0, &lb0);
    a = now();
    must(sqlite3_exec(fdb, "UPDATE t SET v=randomblob(100) WHERE id=12345", 0, 0, 0), "update on fork");
    R(rep, cfg, "w6_one_row_update_on_fork_us", (now() - a) * 1e6);
    fk_stats(f.s, &fb1, &lb1);
    R(rep, cfg, "w6_one_row_update_on_fork_log_bytes", (double)(fb1 - fb0));
    sqlite3_close(fdb);
    fk_discard(f.s, b1);
  } else {
    sqlite3_close(db);
    db = NULL;
    FILE *in = fopen(path, "rb"), *out = fopen(copy, "wb");
    if (!in || !out) die("copy open");
    static char buf[1 << 20];
    size_t n;
    a = now();
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fflush(out);
    fsync(fileno(out));
    R(rep, cfg, "w6_copy_the_file_ms", (now() - a) * 1e3);
    fclose(in);
    fclose(out);
    unlink(copy);
    must(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), "reopen");
  }

  /* W7: reopen */
  if (kind >= 2) {
    uint64_t fb, lb;
    fk_stats(f.s, &fb, &lb);
    R(rep, cfg, "file_bytes", (double)fb);
    R(rep, cfg, "live_bytes", (double)lb);
    fk_stop(&f);
    a = now();
    if (fk_file_open(path, &f.c.real) != 0) die("reopen file");
    f.c.prealloc_end = (uint64_t)f.c.real.size(f.c.real.ctx);
    f.io = wrap(&f.c);
    if (KCHUNK[kind] && fk_prealloc_wrap(&f.io, KCHUNK[kind]) != FK_OK) die("prealloc wrap");
    if (fk_open(&f.io, &f.s) != 0) die("reopen store");
    R(rep, cfg, "w7_fk_open_ms", (now() - a) * 1e3);
    must(fkvfs_register(f.s, "fkb", &f.v), "register");
    must(sqlite3_open_v2("b0", &f.db, SQLITE_OPEN_READWRITE, "fkb"), "open b0");
    a = now();
    if (scalar(f.db, "SELECT count(*) FROM t") != rows) die("rows after reopen");
    R(rep, cfg, "w7_first_count_ms", (now() - a) * 1e3);
    fk_stop(&f);
  } else {
    sqlite3_close(db);
    R(rep, cfg, "file_bytes", (double)fsize(path));
    must(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), "reopen");
    a = now();
    if (scalar(db, "SELECT count(*) FROM t") != rows) die("rows after reopen");
    R(rep, cfg, "w7_first_count_ms", (now() - a) * 1e3);
    sqlite3_close(db);
  }
  unlink(path); unlink(wal); unlink(jr);
  char shm[430];
  snprintf(shm, sizeof shm, "%s-shm", path);
  unlink(shm);
}

/* Sustained commits: NS single-row autocommit transactions in a row, so that a preallocating log goes through many extensions (a 64 MiB
 * chunk holds about 7,900 of them). Reports the latency tail, what the store asked of the disk per commit (zero fill included), and then
 * what a reopen and the first commit after it cost (the layer starts again from the log's end). */
static void sustain(int rep, int kind, const char *dir) {
  const int NS = getenv("BENCH_SUSTAIN_N") ? atoi(getenv("BENCH_SUSTAIN_N")) : 20000;
  const char *cfg = CFG[kind];
  char path[400], wal[420], jr[420], shm[430];
  snprintf(path, sizeof path, "%s/sustain-%d.%s", dir, kind, kind < 2 ? "db" : "fk");
  snprintf(wal, sizeof wal, "%s-wal", path);
  snprintf(jr, sizeof jr, "%s-journal", path);
  snprintf(shm, sizeof shm, "%s-shm", path);
  unlink(path); unlink(wal); unlink(jr); unlink(shm);
  Fk f;
  sqlite3 *db = NULL;
  if (kind >= 2) { fk_start(&f, path, kind == 2, KMODE[kind], KCHUNK[kind]); db = f.db; }
  else {
    must(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL), "open");
    must(sqlite3_exec(db, kind == 1 ? "PRAGMA journal_mode=WAL" : "PRAGMA journal_mode=DELETE", 0, 0, 0), "journal_mode");
    must(sqlite3_exec(db, "PRAGMA synchronous=FULL", 0, 0, 0), "synchronous");
  }
  must(sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)", 0, 0, 0), "create");
  sqlite3_stmt *ins;
  must(sqlite3_prepare_v2(db, "INSERT INTO t(v) VALUES (randomblob(100))", -1, &ins, 0), "prepare");
  double *lat = malloc(sizeof(double) * NS);
  uint64_t fs0 = kind >= 2 ? f.c.fsyncs : 0, pb0 = kind >= 2 ? f.c.pwrite_bytes : 0, rp0 = kind >= 2 ? f.c.real_pwrites : 0;
  double t0 = now();
  for (int i = 0; i < NS; i++) {
    double a = now();
    if (sqlite3_step(ins) != SQLITE_DONE) die("sustain insert");
    sqlite3_reset(ins);
    lat[i] = (now() - a) * 1e6;
  }
  double total = now() - t0, sum = 0;
  for (int i = 0; i < NS; i++) sum += lat[i];
  qsort(lat, NS, sizeof *lat, cmpd);
  R(rep, cfg, "sustain_txn_per_s", NS / total);
  R(rep, cfg, "sustain_mean_us", sum / NS);
  R(rep, cfg, "sustain_p50_us", pct(lat, NS, 0.5));
  R(rep, cfg, "sustain_p99_us", pct(lat, NS, 0.99));
  R(rep, cfg, "sustain_p999_us", pct(lat, NS, 0.999));
  R(rep, cfg, "sustain_max_us", lat[NS - 1]);
  if (kind >= 2) {
    R(rep, cfg, "sustain_fsyncs_per_txn", (double)(f.c.fsyncs - fs0) / NS);
    R(rep, cfg, "sustain_bytes_per_txn", (double)(f.c.pwrite_bytes - pb0) / NS);
    R(rep, cfg, "sustain_pwrites_per_txn", (double)(f.c.real_pwrites - rp0) / NS);
  }
  sqlite3_finalize(ins);
  free(lat);
  if (kind >= 2) {
    fk_stop(&f);
    R(rep, cfg, "sustain_file_bytes_at_rest", (double)fsize(path));
    double a = now();
    fk_start(&f, path, kind == 2, KMODE[kind], KCHUNK[kind]);
    R(rep, cfg, "sustain_reopen_ms", (now() - a) * 1e3);
    db = f.db;
  } else {
    sqlite3_close(db);
    R(rep, cfg, "sustain_file_bytes_at_rest", (double)fsize(path));
    double a = now();
    must(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), "reopen");
    R(rep, cfg, "sustain_reopen_ms", (now() - a) * 1e3);
  }
  for (int i = 0; i < 3; i++) { /* the first commit after an open, then two more for comparison */
    double a = now();
    must(sqlite3_exec(db, "INSERT INTO t(v) VALUES (randomblob(100))", 0, 0, 0), "post-open insert");
    R(rep, cfg, i == 0 ? "sustain_first_commit_after_open_us" : (i == 1 ? "sustain_second_commit_after_open_us" : "sustain_third_commit_after_open_us"), (now() - a) * 1e6);
  }
  if (kind >= 2) fk_stop(&f); else sqlite3_close(db);
  unlink(path); unlink(wal); unlink(jr); unlink(shm);
}

#define NCFG ((int)(sizeof CFG / sizeof *CFG))
int main(int argc, char **argv) {
  const char *only = getenv("BENCH_ONLY"); /* e.g. BENCH_ONLY='file/WAL forkdb/journal': run just those */
  if (argc < 2) { fprintf(stderr, "usage: %s DIR [REPS]\n", argv[0]); return 2; }
  int reps = argc > 2 ? atoi(argv[2]) : 3;
  setvbuf(stdout, NULL, _IOLBF, 0);
  printf("# SQLite %s, batch-atomic compiled in: %s\n", sqlite3_libversion(), sqlite3_compileoption_used("ENABLE_BATCH_ATOMIC_WRITE") ? "yes" : "no");
  for (int rep = 1; rep <= reps; rep++) {
    fsync_floor(rep, argv[1]);
    for (int k = 0; k < NCFG; k++) {
      if (only && !strstr(only, CFG[k])) continue;
      if (getenv("BENCH_MODE") && strcmp(getenv("BENCH_MODE"), "sustain") == 0) sustain(rep, k, argv[1]); else run(rep, k, argv[1]);
    }
  }
  printf("# done\n");
  return 0;
}
