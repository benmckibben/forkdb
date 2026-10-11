/* fkbench.c: measures compaction, recovery (open) time, memory and read cost of the durable layer store on large stores.
 *
 *   fkbench shrink <dir> <live_mib> <rounds> <overwrite_pct>
 *   fkbench forks  <dir> <live_mib> <branches> <write_pct> <random|clustered> <discard_pct>
 *   fkbench depth  <dir> <live_mib> <max_depth>
 *   fkbench churn  <dir> <live_mib> <iterations>
 *
 * Output is "key=value" lines. Build and run with scripts/bench-compact.sh (optimised build, no sanitizers).
 * "cold" means: the file's pages were dropped from the page cache with posix_fadvise(DONTNEED) just before. That drops this
 * kernel's cache only; a virtualised disk (WSL2, a VM) may still have the data cached below it, so cold numbers are a lower
 * bound on what a first read from a real disk costs. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../store/fkstore.h"

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}
static void die(const char *what, int rc) {
  fprintf(stderr, "FAIL: %s rc=%d\n", what, rc);
  exit(2);
}
#define CK(x) do { int rc_ = (x); if (rc_) die(#x, rc_); } while (0)

/* ---- memory ---- */
static long status_kb(const char *key) {
  FILE *f = fopen("/proc/self/status", "r");
  char line[256];
  long v = -1;
  size_t n = strlen(key);
  while (f && fgets(line, sizeof line, f)) if (!strncmp(line, key, n)) { v = atol(line + n); break; }
  if (f) fclose(f);
  return v;
}
static void reset_peak(void) {
  FILE *f = fopen("/proc/self/clear_refs", "w");
  if (f) { fputs("5", f); fclose(f); }
}
static fk_store *g_s;
static void mem(const char *tag) {
  printf("mem_%s_rss_mib=%.1f mem_%s_peak_mib=%.1f", tag, status_kb("VmRSS:") / 1024.0, tag, status_kb("VmHWM:") / 1024.0);
  if (g_s) printf(" mem_%s_index_mib=%.1f", tag, fk_index_bytes(g_s) / 1048576.0); /* what the store itself says its layer tables use */
  printf("\n");
  fflush(stdout);
}

/* ---- files ---- */
static char g_path[1024];
static void drop_cache(void) {
  const char *suffix[] = {"", ".compact"};
  for (int i = 0; i < 2; i++) {
    char p[1100];
    snprintf(p, sizeof p, "%s%s", g_path, suffix[i]);
    int fd = open(p, O_RDONLY);
    if (fd < 0) continue;
    fdatasync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
  }
}
static uint64_t dir_bytes(void) {
  uint64_t t = 0;
  const char *suffix[] = {"", ".compact"};
  for (int i = 0; i < 2; i++) {
    char p[1100];
    struct stat st;
    snprintf(p, sizeof p, "%s%s", g_path, suffix[i]);
    if (stat(p, &st) == 0) t += (uint64_t)st.st_blocks * 512;
  }
  return t;
}
static volatile int g_stop;
static volatile uint64_t g_peak;
static void *sampler(void *a) {
  (void)a;
  while (!g_stop) {
    uint64_t b = dir_bytes();
    if (b > g_peak) g_peak = b;
    usleep(10000);
  }
  return NULL;
}

/* ---- page content: a pure function of (page, version) so that anything can be verified without keeping it ---- */
static void page_fill(uint8_t *buf, uint32_t pg, uint32_t ver) {
  uint64_t x = ((uint64_t)pg << 20) ^ ((uint64_t)ver * 0x9E3779B97F4A7C15ull) ^ 0xD1B54A32D192ED03ull;
  uint64_t *w = (uint64_t *)buf;
  for (int i = 0; i < (int)(FK_PAGE / 8); i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    w[i] = x;
  }
}

static fk_io g_io;
static double open_store(void) {
  double t = now();
  CK(fk_file_open(g_path, &g_io));
  CK(fk_open(&g_io, &g_s));
  return now() - t;
}
static void close_store(void) { fk_close(g_s); fk_file_close(&g_io); g_s = NULL; }

#define BATCH 256u
/* write pages through a multiplicative walk (a bijection on a power-of-two range): pseudo-random order, no repeats */
static uint32_t pow2_ge(uint32_t n) { uint32_t p = 1; while (p < n) p <<= 1; return p; }
static uint32_t walk(uint32_t i, uint32_t m, uint32_t salt) { return (uint32_t)(((uint64_t)i * 2654435761u + salt) & (m - 1)); }

static void commit_pages(uint32_t b, const uint32_t *pgs, const uint32_t *vers, uint32_t n, uint32_t size) {
  static uint8_t buf[FK_PAGE];
  CK(fk_txn_begin(g_s, b));
  for (uint32_t i = 0; i < n; i++) { page_fill(buf, pgs[i], vers[i]); CK(fk_txn_write(g_s, pgs[i], buf)); }
  CK(fk_txn_commit(g_s, size));
}

/* every page of branch b equals fn(pg); returns the number of mismatches; times the scan */
typedef uint32_t (*verfn)(uint32_t pg, void *ctx);
static long verify_all(uint32_t b, uint32_t L, verfn fn, void *ctx, double *ms) {
  uint8_t got[FK_PAGE], want[FK_PAGE];
  long bad = 0;
  double t = now();
  for (uint32_t pg = 0; pg < L; pg++) {
    CK(fk_read(g_s, b, pg, got));
    page_fill(want, pg, fn(pg, ctx));
    if (memcmp(got, want, FK_PAGE)) bad++;
  }
  *ms = now() - t;
  return bad;
}

static void stats(const char *tag) {
  uint64_t f, l;
  CK(fk_stats(g_s, &f, &l));
  printf("%s_file_mib=%.1f %s_live_mib=%.1f\n", tag, f / 1048576.0, tag, l / 1048576.0);
}

static pthread_t g_th;
static void sampler_start(void) { g_stop = 0; g_peak = dir_bytes(); pthread_create(&g_th, NULL, sampler, NULL); }
static uint64_t sampler_stop(void) { g_stop = 1; pthread_join(g_th, NULL); return g_peak; }

static void cpu_ms(double *user, double *sys) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  *user = ru.ru_utime.tv_sec * 1e3 + ru.ru_utime.tv_usec / 1e3;
  *sys = ru.ru_stime.tv_sec * 1e3 + ru.ru_stime.tv_usec / 1e3;
}

static double timed_compact(const char *tag) {
  drop_cache();
  reset_peak();
  sampler_start();
  double u0, s0, u1, s1;
  cpu_ms(&u0, &s0);
  double t = now();
  CK(fk_compact(g_s));
  double ms = now() - t;
  cpu_ms(&u1, &s1);
  uint64_t peak = sampler_stop();
  /* user = checksums and copying in this process; sys = system calls (one pread and one pwrite per page) */
  printf("%s_compact_ms=%.0f %s_compact_cpu_user_ms=%.0f %s_compact_cpu_sys_ms=%.0f %s_peak_disk_mib=%.1f\n", tag, ms, tag, u1 - u0, tag, s1 - s0, tag, peak / 1048576.0);
  mem(tag);
  return ms;
}

/* ================= shrink ================= */
static uint32_t shrink_ver(uint32_t pg, void *ctx) { return ((uint8_t *)ctx)[pg]; }
static int sc_shrink(double live_mib, int rounds, double pct) {
  uint32_t L = (uint32_t)(live_mib * 1048576.0 / FK_PAGE);
  uint32_t M = pow2_ge(L);
  uint8_t *ver = calloc(L, 1);
  printf("scenario=shrink live_mib=%.0f pages=%u rounds=%d overwrite_pct=%.0f\n", live_mib, L, rounds, pct);
  open_store();
  uint32_t pgs[BATCH], vs[BATCH];
  double t = now();
  uint64_t written = 0;
  for (int r = 0; r < rounds; r++) {
    uint32_t n_total = r == 0 ? L : (uint32_t)(L * pct / 100.0);
    uint32_t n = 0, i = 0, salt = 977u * (uint32_t)r;
    for (uint32_t done = 0; done < n_total;) {
      n = 0;
      while (n < BATCH && done < n_total) {
        uint32_t pg = r == 0 ? done : walk(i++, M, salt);
        if (pg >= L) continue;
        pgs[n] = pg; vs[n] = ++ver[pg]; n++; done++;
      }
      commit_pages(0, pgs, vs, n, L);
      written += n;
    }
  }
  double ms = now() - t;
  printf("build_ms=%.0f build_pages=%llu build_mib_per_s=%.0f\n", ms, (unsigned long long)written, written * (double)FK_PAGE / 1048576.0 / (ms / 1000.0));
  stats("before");
  mem("built");
  close_store();
  drop_cache();
  printf("open_cold_uncompacted_ms=%.0f\n", open_store());
  mem("open_uncompacted");
  double c = timed_compact("shrink");
  stats("after");
  double vms;
  drop_cache();
  long bad = verify_all(0, L, shrink_ver, ver, &vms);
  printf("verify_cold_ms=%.0f verify_cold_mib_per_s=%.0f verify_mismatches=%ld\n", vms, live_mib / (vms / 1000.0), bad);
  int did = -1;
  CK(fk_compact_if_needed(g_s, 0, &did));
  printf("second_compact_if_needed_did=%d\n", did);
  close_store();
  drop_cache();
  printf("open_cold_compacted_ms=%.0f\n", open_store());
  mem("open_compacted");
  bad += verify_all(0, L < 20000 ? L : 20000, shrink_ver, ver, &vms);
  close_store();
  printf("RESULT shrink ok=%d compact_ms=%.0f\n", bad == 0, c);
  return bad != 0;
}

/* ================= forks ================= */
typedef struct { uint32_t L, nb, nw; int rnd; uint32_t M; } FCtx;
static uint32_t fork_pg(const FCtx *c, uint32_t b, uint32_t i) { return c->rnd ? walk(i, c->M, 131u * b) % c->L : i % c->L; }
static int sc_forks(double live_mib, uint32_t nb, double pct, int rnd, double dpct) {
  FCtx c = {.L = (uint32_t)(live_mib * 1048576.0 / FK_PAGE), .nb = nb, .rnd = rnd};
  c.nw = (uint32_t)(c.L * pct / 100.0);
  c.M = pow2_ge(c.L);
  printf("scenario=forks live_mib=%.0f pages=%u branches=%u write_pct=%.2f layout=%s discard_pct=%.0f pages_per_branch=%u\n", live_mib, c.L, nb, pct, rnd ? "random" : "clustered", dpct, c.nw);
  open_store();
  uint32_t pgs[BATCH], vs[BATCH];
  for (uint32_t pg = 0; pg < c.L;) {
    uint32_t n = 0;
    while (n < BATCH && pg < c.L) { pgs[n] = pg; vs[n] = 1; n++; pg++; }
    commit_pages(0, pgs, vs, n, c.L);
  }
  mem("base");
  double t = now(), tw = 0;
  for (uint32_t k = 1; k <= nb; k++) {
    uint32_t dst;
    double t0 = now();
    CK(fk_fork(g_s, 0, &dst));
    double tf = now() - t0;
    (void)tf;
    for (uint32_t i = 0; i < c.nw;) {
      uint32_t n = 0;
      while (n < BATCH && i < c.nw) { pgs[n] = fork_pg(&c, k, i); vs[n] = 100 + k; n++; i++; }
      /* a batch must not contain a page twice (the walk is a bijection on M but "% L" can fold); dedupe cheaply */
      for (uint32_t a = 0; a < n; a++) for (uint32_t d = 0; d < a; d++) if (pgs[a] == pgs[d]) { pgs[a] = UINT32_MAX; break; }
      uint32_t m = 0;
      for (uint32_t a = 0; a < n; a++) if (pgs[a] != UINT32_MAX) { pgs[m] = pgs[a]; vs[m] = vs[a]; m++; }
      double t1 = now();
      commit_pages(dst, pgs, vs, m, c.L);
      tw += now() - t1;
    }
  }
  printf("forks_build_ms=%.0f\n", now() - t);
  mem("forked");
  stats("before");
  uint32_t dropped = 0;
  for (uint32_t k = 1; k <= nb; k++)
    if ((double)(k % 100) < dpct) { CK(fk_discard(g_s, k)); dropped++; }
  printf("discarded=%u\n", dropped);
  double c1 = timed_compact("forks");
  stats("after");
  mem("after_compact");
  /* verify every surviving branch: all pages it wrote, plus a stride sample of the rest */
  long bad = 0, checked = 0;
  uint8_t got[FK_PAGE], want[FK_PAGE];
  for (uint32_t k = 1; k <= nb; k++) {
    if (!fk_branch_alive(g_s, k)) continue;
    uint8_t *w = calloc(c.L, 1);
    for (uint32_t i = 0; i < c.nw; i++) w[fork_pg(&c, k, i)] = 1;
    /* a stride of 97 from an unwritten page would skip writes that land in that gap */
    for (uint32_t pg = 0; pg < c.L; pg++) if (w[pg]) {
      CK(fk_read(g_s, k, pg, got));
      page_fill(want, pg, 100 + k);
      bad += memcmp(got, want, FK_PAGE) != 0;
      checked++;
    }
    for (uint32_t pg = 0; pg < c.L; pg += 97) if (!w[pg]) {
      CK(fk_read(g_s, k, pg, got));
      page_fill(want, pg, 1);
      bad += memcmp(got, want, FK_PAGE) != 0;
      checked++;
    }
    free(w);
  }
  printf("verify_checked=%ld verify_mismatches=%ld\n", checked, bad);
  close_store();
  drop_cache();
  printf("open_cold_compacted_ms=%.0f\n", open_store());
  mem("open_compacted");
  close_store();
  printf("RESULT forks ok=%d compact_ms=%.0f\n", bad == 0, c1);
  return bad != 0;
}

/* ================= depth ================= */
static double read_avg_ns(uint32_t b, uint32_t lo, uint32_t hi, int n) {
  uint8_t buf[FK_PAGE];
  uint64_t x = 88172645463325252ull;
  double t = now();
  for (int i = 0; i < n; i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    CK(fk_read(g_s, b, lo + (uint32_t)(x % (hi - lo)), buf));
  }
  return (now() - t) * 1e6 / n;
}
static int sc_depth(double live_mib, int maxd) {
  uint32_t L = (uint32_t)(live_mib * 1048576.0 / FK_PAGE);
  printf("scenario=depth live_mib=%.0f pages=%u max_depth=%d\n", live_mib, L, maxd);
  open_store();
  uint32_t pgs[BATCH], vs[BATCH];
  for (uint32_t pg = 0; pg < L;) {
    uint32_t n = 0;
    while (n < BATCH && pg < L) { pgs[n] = pg; vs[n] = 1; n++; pg++; }
    commit_pages(0, pgs, vs, n, L);
  }
  int depths[] = {1, 10, 100, 300, 1000};
  int d = 0, rc = 0;
  double tfork = 0, tcommit = 0;
  uint32_t dst;
  /* warm the page cache for the base so the per-read cost below is the lookup, not the disk */
  (void)read_avg_ns(0, 4096, L, 200000);
  printf("read_ns_depth0_warm=%.0f\n", read_avg_ns(0, 4096, L, 200000));
  for (size_t k = 0; k < sizeof depths / sizeof *depths && depths[k] <= maxd; k++) {
    while (d < depths[k]) {
      pgs[0] = 1 + (uint32_t)d % 2000; vs[0] = 2 + (uint32_t)d;
      double t0 = now();
      commit_pages(0, pgs, vs, 1, L);
      tcommit += now() - t0;
      t0 = now();
      rc = fk_fork(g_s, 0, &dst);
      tfork += now() - t0;
      if (rc) break;
      CK(fk_discard(g_s, dst));
      d++;
    }
    if (rc) break;
    printf("read_ns_depth%d_warm=%.0f\n", d, read_avg_ns(0, 4096, L, 200000));
  }
  printf("one_page_commit_avg_ms=%.2f fork_avg_us=%.1f branch_ids_used=%u\n", tcommit / (d ? d : 1), tfork * 1000 / (d ? d : 1), fk_branch_count(g_s));
  /* the old limit was 1,023 forks per store: go on past it (ids of discarded branches are reused), but stop after a bound */
  for (int extra = 0; extra < 2500 && !rc; extra++) {
    pgs[0] = 1; vs[0] = 3;
    commit_pages(0, pgs, vs, 1, L);
    rc = fk_fork(g_s, 0, &dst);
    if (rc) break;
    CK(fk_discard(g_s, dst));
    d++;
  }
  printf("forks_done=%d first_failure_rc=%d branch_ids_used=%u\n", d, rc, fk_branch_count(g_s));
  mem("depth_deep");
  printf("read_ns_deep_warm=%.0f\n", read_avg_ns(0, 4096, L, 200000));
  double c = timed_compact("depth");
  printf("read_ns_after_compact_warm=%.0f\n", read_avg_ns(0, 4096, L, 200000));
  stats("after");
  uint32_t dst2 = 0;
  rc = fk_fork(g_s, 0, &dst2);
  printf("fork_after_compact_rc=%d branch_ids_used=%u\n", rc, fk_branch_count(g_s));
  close_store();
  printf("RESULT depth ok=1 compact_ms=%.0f\n", c);
  return 0;
}

/* ================= churn ================= */
/* The workflow ids are reused for: per iteration, write a page on main, fork it, write two pages on the fork, discard the fork. */
static uint32_t churn_ver(uint32_t pg, void *ctx) { return ((uint32_t *)ctx)[pg]; }
static int sc_churn(double live_mib, int iters) {
  uint32_t L = (uint32_t)(live_mib * 1048576.0 / FK_PAGE);
  uint32_t *ver = calloc(L, sizeof *ver);
  printf("scenario=churn live_mib=%.0f pages=%u iterations=%d\n", live_mib, L, iters);
  open_store();
  uint32_t pgs[BATCH], vs[BATCH];
  for (uint32_t pg = 0; pg < L;) {
    uint32_t n = 0;
    while (n < BATCH && pg < L) { pgs[n] = pg; vs[n] = 1; ver[pg] = 1; n++; pg++; }
    commit_pages(0, pgs, vs, n, L);
  }
  mem("base");
  uint32_t dst = 0, maxid = 0;
  int rc = 0, report = iters >= 10 ? iters / 10 : 1;
  double t0 = now();
  for (int it = 1; it <= iters && !rc; it++) {
    uint32_t pg = 1 + (uint32_t)it % (L - 100);
    pgs[0] = pg; vs[0] = 100u + (uint32_t)it; ver[pg] = vs[0];
    commit_pages(0, pgs, vs, 1, L);
    rc = fk_fork(g_s, 0, &dst);
    if (rc) break;
    if (dst > maxid) maxid = dst;
    pgs[0] = L - 1; pgs[1] = L - 2; vs[0] = 5000000u + (uint32_t)it; vs[1] = vs[0] + 1;
    commit_pages(dst, pgs, vs, 2, L);
    CK(fk_discard(g_s, dst));
    if (it % report == 0) {
      uint64_t f, l;
      CK(fk_stats(g_s, &f, &l));
      printf("iter=%d highest_id=%u file_mib=%.1f live_mib=%.1f index_mib=%.2f read_ns_main=%.0f elapsed_s=%.0f\n", it, maxid, f / 1048576.0, l / 1048576.0, fk_index_bytes(g_s) / 1048576.0, read_avg_ns(0, 4096, L - 100, 100000), (now() - t0) / 1000);
    }
  }
  printf("forks_done_rc=%d highest_id_used=%u\n", rc, maxid);
  double c = timed_compact("churn");
  stats("after");
  printf("after_compact_index_mib=%.2f read_ns_main_after=%.0f\n", fk_index_bytes(g_s) / 1048576.0, read_avg_ns(0, 4096, L - 100, 100000));
  double vms;
  long bad = verify_all(0, L, churn_ver, ver, &vms);
  printf("verify_main_ms=%.0f verify_mismatches=%ld\n", vms, bad);
  close_store();
  free(ver);
  printf("RESULT churn ok=%d compact_ms=%.0f\n", bad == 0 && rc == 0, c);
  return bad != 0 || rc != 0;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: see the top of fkbench.c\n"); return 1; }
  snprintf(g_path, sizeof g_path, "%s/store.fk", argv[2]);
  unlink(g_path);
  setvbuf(stdout, NULL, _IOLBF, 0);
  int rc = 1;
  if (!strcmp(argv[1], "shrink") && argc == 6) rc = sc_shrink(atof(argv[3]), atoi(argv[4]), atof(argv[5]));
  else if (!strcmp(argv[1], "forks") && argc == 8) rc = sc_forks(atof(argv[3]), (uint32_t)atoi(argv[4]), atof(argv[5]), !strcmp(argv[6], "random"), atof(argv[7]));
  else if (!strcmp(argv[1], "churn") && argc == 5) rc = sc_churn(atof(argv[3]), atoi(argv[4]));
  else if (!strcmp(argv[1], "depth") && argc == 5) rc = sc_depth(atof(argv[3]), atoi(argv[4]));
  else fprintf(stderr, "bad arguments\n");
  unlink(g_path);
  return rc;
}
