/* oom_harness.h: controlled OOM injection for testing error-recovery paths.
 *
 * Compile every translation unit of the test with -DOOM_INJECT and include
 * this header after the system headers (so the macros below do not rewrite
 * the libc prototypes). malloc/calloc/realloc then go through the wrappers.
 *
 * The counters are defined once, in the test driver. They must not be static:
 * a static definition here would give each translation unit its own counter,
 * and oom_fail_after() in the driver would not affect allocations in the store.
 *
 * oom_fail_after(n) makes the next allocation after n successes return NULL.
 * oom_reset() restores normal behaviour.
 */
#ifndef OOM_HARNESS_H
#define OOM_HARNESS_H

#include <stddef.h>

#ifdef OOM_INJECT
#include <stdlib.h>

extern volatile int oom_counter; /* -1 = disabled; -2 = injection fired; >= 0 = fail after this many successes */
extern volatile int oom_total;

static inline void oom_reset(void) { oom_counter = -1; oom_total = 0; }
static inline void oom_fail_after(int n) { oom_counter = n; oom_total = 0; }
static inline int oom_did_fail(void) { return oom_counter == -2; }

static inline void *oom_malloc(size_t n) {
  if (oom_counter < 0) return malloc(n);
  if (oom_total++ >= oom_counter) { oom_counter = -2; return NULL; }
  return malloc(n);
}
static inline void *oom_calloc(size_t n, size_t sz) {
  if (oom_counter < 0) return calloc(n, sz);
  if (oom_total++ >= oom_counter) { oom_counter = -2; return NULL; }
  return calloc(n, sz);
}
static inline void *oom_realloc(void *p, size_t n) {
  if (oom_counter < 0) return realloc(p, n);
  if (oom_total++ >= oom_counter) { oom_counter = -2; return NULL; }
  return realloc(p, n);
}

#define malloc(n)  oom_malloc(n)
#define calloc(n,s) oom_calloc(n,s)
#define realloc(p,n) oom_realloc(p,n)

#endif /* OOM_INJECT */
#endif /* OOM_HARNESS_H */
