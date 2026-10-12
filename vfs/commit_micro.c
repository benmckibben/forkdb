/* commit_micro: what one forkdb commit costs at the file level, without SQLite or the store. One "commit" is what the store does for a
 * single-row SQLite transaction: two page records (4120 bytes each) and a commit record (about 40 bytes), then a sync. Each pattern varies
 * one thing. usage: commit_micro DIR [REPS] [COMMITS]      prints: M <rep> <pattern> p50_us <v> / p99_us <v> */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void die(const char *m) { perror(m); exit(2); }

#define REC1 4120u
#define REC2 4120u
#define REC3 40u
#define COMMIT_BYTES (REC1 + REC2 + REC3)
#define PRE (64u << 20)

enum { APPEND = 1, PREZERO = 2, FALLOC = 4, ONE_WRITE = 8, DSYNC = 16, FDATASYNC = 32 };
typedef struct { const char *name; int flags; } Pat;
static const Pat PATS[] = {
  {"A append, 3 writes, fdatasync", APPEND | FDATASYNC},
  {"B append, 1 write, fdatasync", APPEND | ONE_WRITE | FDATASYNC},
  {"C zero-prealloc, 3 writes, fdatasync", PREZERO | FDATASYNC},
  {"D zero-prealloc, 1 write, fdatasync", PREZERO | ONE_WRITE | FDATASYNC},
  {"E fallocate, 1 write, fdatasync", FALLOC | ONE_WRITE | FDATASYNC},
  {"F append, 1 write, O_DSYNC", APPEND | ONE_WRITE | DSYNC},
  {"G zero-prealloc, 1 write, O_DSYNC", PREZERO | ONE_WRITE | DSYNC},
  {"H zero-prealloc, 1 write, fsync", PREZERO | ONE_WRITE},
};

static void run(int rep, const Pat *p, const char *dir, int n) {
  char path[400];
  snprintf(path, sizeof path, "%s/micro.bin", dir);
  unlink(path);
  int fl = O_CREAT | O_RDWR | O_TRUNC | ((p->flags & DSYNC) ? O_DSYNC : 0);
  int fd = open(path, fl, 0644);
  if (fd < 0) die("open");
  uint8_t *buf = calloc(1, COMMIT_BYTES);
  memset(buf, 0x5a, COMMIT_BYTES);
  if (p->flags & PREZERO) {
    uint8_t *z = calloc(1, 1u << 20);
    for (uint32_t o = 0; o < PRE; o += 1u << 20) if (pwrite(fd, z, 1u << 20, o) != (ssize_t)(1u << 20)) die("prezero");
    if (fsync(fd)) die("fsync");
    free(z);
  }
  if (p->flags & FALLOC) { if (fallocate(fd, 0, 0, PRE)) die("fallocate"); if (fsync(fd)) die("fsync"); }
  double *lat = malloc(sizeof(double) * (size_t)n);
  uint64_t off = 0;
  for (int i = 0; i < n; i++) {
    buf[0] = (uint8_t)i;
    double t = now();
    if (p->flags & ONE_WRITE) {
      if (pwrite(fd, buf, COMMIT_BYTES, (off_t)off) != (ssize_t)COMMIT_BYTES) die("pwrite");
    } else {
      if (pwrite(fd, buf, REC1, (off_t)off) != (ssize_t)REC1) die("pwrite1");
      if (pwrite(fd, buf + REC1, REC2, (off_t)(off + REC1)) != (ssize_t)REC2) die("pwrite2");
      if (pwrite(fd, buf + REC1 + REC2, REC3, (off_t)(off + REC1 + REC2)) != (ssize_t)REC3) die("pwrite3");
    }
    if (!(p->flags & DSYNC)) { if ((p->flags & FDATASYNC) ? fdatasync(fd) : fsync(fd)) die("sync"); }
    lat[i] = (now() - t) * 1e6;
    off += COMMIT_BYTES;
  }
  close(fd);
  unlink(path);
  qsort(lat, (size_t)n, sizeof *lat, cmpd);
  printf("M %d %s p50_us %.2f\nM %d %s p99_us %.2f\n", rep, p->name, lat[(n - 1) / 2], rep, p->name, lat[(int)((n - 1) * 0.99)]);
  free(lat);
  free(buf);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s DIR [REPS] [COMMITS]\n", argv[0]); return 2; }
  int reps = argc > 2 ? atoi(argv[2]) : 3, n = argc > 3 ? atoi(argv[3]) : 2000;
  setvbuf(stdout, NULL, _IOLBF, 0);
  for (int r = 1; r <= reps; r++)
    for (size_t k = 0; k < sizeof PATS / sizeof *PATS; k++) run(r, &PATS[k], argv[1], n);
  printf("# done\n");
  return 0;
}
