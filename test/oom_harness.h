/* oom_harness.h: controlled OOM injection for testing error-recovery paths.
 *
 * Use: #include "oom_harness.h" and call oom_fail_after(n) to make the
 * next allocation after n successes fail with NULL. Call oom_reset() to
 * restore normal behaviour.
 *
 * The interception works by #define'ing malloc/calloc/realloc to point at
 * the harness wrappers. Include this header last (after all system headers)
 * in test files that use the injection, and only in those files.
 */
#ifndef OOM_HARNESS_H
#define OOM_HARNESS_H

#include <stddef.h>

#ifdef OOM_INJECT
#include <stdlib.h>

static volatile int oom_counter = -1; /* -1 = disabled; >= 0 = fail after this many successes */
static volatile int oom_total = 0;

static void oom_reset(void) { oom_counter = -1; oom_total = 0; }
static void oom_fail_after(int n) { oom_counter = n; oom_total = 0; }
static int oom_did_fail(void) { return oom_counter == -2; }

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
