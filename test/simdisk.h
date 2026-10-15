/* simdisk: an in-memory "file" with a volatile write cache, for power-loss testing.
 *
 * Model (deliberately pessimistic, see docs/DURABLE-STORE.md):
 *  - pwrite/truncate go to a pending list; the process sees them at once ("visible" image).
 *  - fsync makes everything pending for that file durable. Nothing else does.
 *  - On a power cut, the surviving image is the durable image plus ANY subset of the pending operations, applied in
 *    ANY order, and a surviving write may be torn: only some of its 512-byte sectors, or only a byte prefix.
 *  - Bytes between the old end of file and a surviving write may be zeros or garbage.
 *  - There are two files: the store's file and a temporary sibling used by compaction. Which file carries the store's name is
 *    directory state: a rename is visible at once but is durable only after sync_dir. At a power cut, a pending rename may or
 *    may not have happened.
 */
#ifndef SIMDISK_H
#define SIMDISK_H

#include <stddef.h>
#include <stdint.h>

#include "../store/fkstore.h"

typedef struct { uint64_t s; } Rng;
void rng_seed(Rng *r, uint64_t seed);
uint64_t rng_next(Rng *r);
uint32_t rng_below(Rng *r, uint32_t n);

typedef struct SimOp { int kind; /* 0 pwrite, 1 truncate */ uint64_t off, len; uint8_t *data; } SimOp;

typedef struct SimFile {
  uint8_t *dur, *vis;
  uint64_t dur_len, vis_len, dcap, vcap;
  SimOp *pend;
  int npend, cappend;
} SimFile;

struct SimDisk;
typedef struct { struct SimDisk *d; int tmp; } SimHandle;

typedef struct SimDisk {
  SimFile f[2];
  int vis_main, dur_main; /* which of f[] carries the store's name now, and after a power cut that loses pending directory changes */
  long events;      /* pwrite, truncate, fsync, create, rename and sync_dir calls so far */
  long crash_at;    /* the power is cut just before event number crash_at (-1: never) */
  int dead;         /* power is off: every call fails */
  long fsync_fail_at; /* the fsync or sync_dir event with this number returns -1 and leaves everything pending (-1: never) */
  int nosync;       /* fsync and sync_dir succeed and make nothing durable: a store opened with FK_FILE_NOSYNC. Every write stays pending until the power cut */
  SimHandle hm, ht;
} SimDisk;

void sim_init(SimDisk *d, const uint8_t *image, uint64_t len);
void sim_free(SimDisk *d);
fk_io sim_io(SimDisk *d);
/* The store file's durable image (what a clean power-off with nothing pending would leave). */
const uint8_t *sim_durable(const SimDisk *d, uint64_t *len);

/* What the process sees (the volatile image): what a restart after the death of the process, with the operating system still up, would find. */
const uint8_t *sim_visible(const SimDisk *d, uint64_t *len);

/* Calls cb for every post-crash image of the store's file (a subset of the pending operations, in some order, possibly torn,
 * for each way a pending rename may have turned out). Exhaustive over subsets when there are at most 8 pending operations,
 * otherwise a random sample. */
typedef void (*sim_image_cb)(void *ctx, const uint8_t *img, uint64_t len);
void sim_images(const SimDisk *d, Rng *r, sim_image_cb cb, void *ctx);

/* The images a power cut most plausibly leaves with no syncs at all: the first m pending writes of the store's file in the order they were made, the last of them
 * whole or torn, for about 'samples' values of m spread over the whole run (all of them when there are fewer). Covers the case where the operating system happened to
 * write back in order and stopped somewhere; sim_images covers the case where it did not. */
void sim_images_prefix(const SimDisk *d, Rng *r, int samples, sim_image_cb cb, void *ctx);

#endif
