/* fkx core: the branch layer over the store and the VFS. See docs/FKX-DESIGN.md. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "../vfs/fkvfs.h"
#include "fkx_internal.h"

#define META_FIXED 40u
#define MAGIC0 'F'
#define MAGIC1 'K'
#define MAGIC2 'X'
#define MAGIC3 '1'

typedef struct Trk { void *h; void (*fn)(void *); struct Trk *next; } Trk; /* an open handle fkx_close has to abort */

struct fkx {
  Trk *trk;
  fk_io io;
  int own_file, own_prealloc;
  fk_store *s;
  FkVfs *vfs;
  char vfs_name[32];
  char root_name[FKX_NAME_MAX + 1];
  unsigned undo_keep;
  uint64_t (*random64)(void *ctx);
  void *random_ctx;
  char err[256];
  int store_rc, sqlite_rc;
};

/* ---------- errors ---------- */
static int failf(fkx *x, int code, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(x->err, sizeof x->err, fmt, ap);
  va_end(ap);
  return code;
}
static int store_fail(fkx *x, int rc, const char *what) {
  x->store_rc = rc;
  int code = rc == FK_EBUSY ? FKX_EBUSY : rc == FK_ELIMIT ? FKX_ELIMIT : rc == FK_EVERSION ? FKX_EVERSION : rc == FK_ENOENT ? FKX_ENOENT : rc == FK_ENOMEM ? FKX_ENOMEM : rc == FK_EINVAL ? FKX_EINVAL : FKX_ESTORE;
  return failf(x, code, "%s failed in the store (code %d)%s", what, rc, code == FKX_ESTORE ? ": the store is fail-stop, close and reopen" : "");
}
const char *fkx_errmsg(const fkx *x) { return x->err; }
int fkx_store_rc(const fkx *x) { return x->store_rc; }
int fkx_sqlite_rc(const fkx *x) { return x->sqlite_rc; }
const char *fkx_vfs_name(const fkx *x) { return x->vfs_name; }
const fk_store *fkx_store(const fkx *x) { return x->s; }
fk_store *fkx_store_rw(fkx *x) { return x->s; }
struct FkVfs *fkx_vfs(fkx *x) { return x->vfs; }

/* ---------- metadata codec ---------- */
static void p64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t g64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

static int name_ok(const char *n) {
  size_t len = n ? strlen(n) : 0;
  if (len == 0 || len > FKX_NAME_MAX || n[0] == '#') return 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)n[i];
    if (!(isalnum(c) || c == '.' || c == '_' || c == '-' || c == '/')) return 0;
  }
  return 1;
}

static size_t meta_encode(const Meta *m, uint8_t *b) {
  memset(b, 0, META_FIXED);
  b[0] = MAGIC0; b[1] = MAGIC1; b[2] = MAGIC2; b[3] = MAGIC3;
  b[4] = (uint8_t)m->kind;
  b[5] = (uint8_t)(m->ro ? 1 : 0);
  b[6] = (uint8_t)m->nlen;
  b[7] = (uint8_t)m->nundo;
  p64(b + 8, m->uid); p64(b + 16, m->origin); p64(b + 24, m->base); p64(b + 32, m->owner);
  memcpy(b + META_FIXED, m->name, m->nlen);
  size_t o = META_FIXED + m->nlen;
  for (unsigned i = 0; i < m->nundo; i++) { p64(b + o, m->undo[i][0]); p64(b + o + 8, m->undo[i][1]); o += 16; }
  return o;
}

/* 0 if b is a well-formed blob */
static int meta_decode(const uint8_t *b, size_t len, Meta *m) {
  if (len < META_FIXED || b[0] != MAGIC0 || b[1] != MAGIC1 || b[2] != MAGIC2 || b[3] != MAGIC3) return -1;
  memset(m, 0, sizeof *m);
  if (b[4] < FKX_ROOT || b[4] > FKX_UNDO) return -1;
  m->kind = (fkx_kind)b[4];
  m->ro = b[5] & 1;
  m->nlen = b[6];
  m->nundo = b[7];
  if (m->nlen > FKX_NAME_MAX || m->nundo > FKX_UNDO_KEEP_MAX || len != META_FIXED + m->nlen + 16u * m->nundo) return -1;
  int named = m->kind == FKX_ROOT || m->kind == FKX_FORK || m->kind == FKX_SNAPSHOT;
  if (named != (m->nlen > 0)) return -1;
  if ((m->kind == FKX_SNAPSHOT || m->kind == FKX_BASE || m->kind == FKX_UNDO) != m->ro) return -1;
  if ((m->kind == FKX_BASE || m->kind == FKX_UNDO || m->kind == FKX_SNAPSHOT) && m->nundo) return -1;
  m->uid = g64(b + 8); m->origin = g64(b + 16); m->base = g64(b + 24); m->owner = g64(b + 32);
  memcpy(m->name, b + META_FIXED, m->nlen);
  m->name[m->nlen] = 0;
  if (named) { char tmp[FKX_NAME_MAX + 1]; memcpy(tmp, m->name, m->nlen + 1); if (!name_ok(tmp)) return -1; }
  if (!m->uid) return -1;
  size_t o = META_FIXED + m->nlen;
  for (unsigned i = 0; i < m->nundo; i++) { m->undo[i][0] = g64(b + o); m->undo[i][1] = g64(b + o + 8); o += 16; }
  return 0;
}

/* 1 if the branch has no metadata at all, 0 if loaded, -1 if present and malformed */
static int meta_load(const fkx *x, uint32_t id, Meta *m) {
  uint8_t b[FK_META_MAX];
  size_t len = 0;
  if (fk_meta_get(x->s, id, b, sizeof b, &len) != FK_OK) return -1;
  if (len == 0) return 1;
  if (len > sizeof b) return -1;
  return meta_decode(b, len, m);
}

static int meta_store(fkx *x, uint32_t id, const Meta *m) {
  uint8_t b[FK_META_MAX];
  size_t n = meta_encode(m, b);
  if (n > FK_META_MAX) return failf(x, FKX_ELIMIT, "metadata of %zu bytes does not fit in %u", n, FK_META_MAX);
  int rc = fk_meta_set(x->s, id, b, n);
  return rc == FK_OK ? FKX_OK : store_fail(x, rc, "writing branch metadata");
}

/* ---------- lookups (a scan of the live branches' metadata: at most FK_MAX_BRANCHES small in-memory blobs, so no index to go stale) ---------- */
static int find_uid(const fkx *x, uint64_t uid, uint32_t *id, Meta *m) {
  Meta t;
  uint32_t n = fk_branch_count(x->s);
  if (!uid) return 0;
  for (uint32_t i = 0; i < n; i++) {
    if (!fk_branch_alive(x->s, i) || meta_load(x, i, &t) != 0) continue;
    if (t.uid == uid) { if (id) *id = i; if (m) *m = t; return 1; }
  }
  return 0;
}
static int find_name(const fkx *x, const char *name, uint32_t *id, Meta *m) {
  Meta t;
  uint32_t n = fk_branch_count(x->s);
  for (uint32_t i = 0; i < n; i++) {
    if (!fk_branch_alive(x->s, i) || meta_load(x, i, &t) != 0) continue;
    if ((t.kind == FKX_ROOT || t.kind == FKX_FORK || t.kind == FKX_SNAPSHOT) && strcmp(t.name, name) == 0) { if (id) *id = i; if (m) *m = t; return 1; }
  }
  return 0;
}
static int resolve(fkx *x, const char *ref, uint32_t *id, Meta *m) {
  if (!ref) return failf(x, FKX_EINVAL, "no branch given");
  if (ref[0] == '#') {
    if (strlen(ref) != 17) return failf(x, FKX_EINVAL, "'%s' is not a branch uid (# and 16 hex digits)", ref);
    uint64_t u = 0;
    for (int i = 1; i < 17; i++) {
      int c = tolower((unsigned char)ref[i]);
      int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (d < 0) return failf(x, FKX_EINVAL, "'%s' is not a branch uid (# and 16 hex digits)", ref);
      u = u << 4 | (uint64_t)d;
    }
    if (!find_uid(x, u, id, m)) return failf(x, FKX_ENOENT, "no branch with uid %s", ref);
    return FKX_OK;
  }
  if (!find_name(x, ref, id, m)) return failf(x, FKX_ENOENT, "no branch named '%s'", ref);
  return FKX_OK;
}
static uint32_t alive_count(const fkx *x) {
  uint32_t c = 0, n = fk_branch_count(x->s);
  for (uint32_t i = 0; i < n; i++) c += fk_branch_alive(x->s, i) ? 1u : 0u;
  return c;
}
static uint32_t refs(const fkx *x, uint32_t id) { return fkvfs_branch_refs(x->vfs, id); }

static uint64_t sys_random(void *ctx) {
  (void)ctx;
  uint64_t v = 0;
  if (getrandom(&v, sizeof v, 0) != (ssize_t)sizeof v) v = (uint64_t)random() << 32 ^ (uint64_t)random();
  return v;
}
static uint64_t new_uid(fkx *x) {
  for (;;) {
    uint64_t u = x->random64(x->random_ctx);
    if (u && !find_uid(x, u, NULL, NULL)) return u;
  }
}

/* ---------- options, open, close ---------- */
void fkx_options_default(fkx_options *o) {
  memset(o, 0, sizeof *o);
  o->vfs_name = "forkdb";
  o->root_name = "main";
  o->prealloc_chunk = FK_PREALLOC_DEFAULT;
  o->undo_keep = 8;
  o->create = 1;
}

static int gc_impl(fkx *x, fkx_gc_report *rep);
static void fkx_install_guard(fkx *x);

int fkx_check(fkx *x, char *buf, size_t n) {
  uint32_t cnt = fk_branch_count(x->s);
  int roots = 0;
  Meta a, b;
  for (uint32_t i = 0; i < cnt; i++) {
    if (!fk_branch_alive(x->s, i)) continue;
    int rc = meta_load(x, i, &a);
    if (rc != 0) { snprintf(buf, n, "branch %u has no valid fkx metadata", i); return -1; }
    if (a.kind == FKX_ROOT) { roots++; if (i != 0) { snprintf(buf, n, "the root is branch %u, not 0", i); return -1; } }
    for (uint32_t j = i + 1; j < cnt; j++) {
      if (!fk_branch_alive(x->s, j) || meta_load(x, j, &b) != 0) continue;
      if (a.uid == b.uid) { snprintf(buf, n, "branches %u and %u have the same uid", i, j); return -1; }
      int na = a.nlen > 0 && a.kind != FKX_BASE && a.kind != FKX_UNDO, nb = b.nlen > 0;
      if (na && nb && strcmp(a.name, b.name) == 0) { snprintf(buf, n, "branches %u and %u are both named '%s'", i, j, a.name); return -1; }
    }
    if (a.kind == FKX_FORK) {
      Meta bm;
      uint32_t bid;
      if (!find_uid(x, a.base, &bid, &bm) || bm.kind != FKX_BASE || bm.owner != a.uid) { snprintf(buf, n, "fork '%s' (%u) has no base of its own", a.name, i); return -1; }
    }
    if (a.kind == FKX_ROOT || a.kind == FKX_FORK) {
      for (unsigned e = 0; e < a.nundo; e++)
        for (int k = 0; k < 2; k++) {
          Meta um;
          if (!find_uid(x, a.undo[e][k], NULL, &um) || um.kind != FKX_UNDO || um.owner != a.uid) { snprintf(buf, n, "'%s' lists an undo snapshot that is missing or not its own", a.name); return -1; }
        }
    }
    if (a.kind == FKX_BASE || a.kind == FKX_UNDO) {
      Meta om;
      if (!find_uid(x, a.owner, NULL, &om)) { snprintf(buf, n, "internal branch %u (kind %d) has no owner", i, a.kind); return -1; }
      int listed = 0;
      if (a.kind == FKX_BASE) listed = om.kind == FKX_FORK && om.base == a.uid;
      else for (unsigned e = 0; e < om.nundo; e++) listed |= om.undo[e][0] == a.uid || om.undo[e][1] == a.uid;
      if (!listed) { snprintf(buf, n, "internal branch %u (kind %d) is not listed by its owner", i, a.kind); return -1; }
    }
  }
  if (roots != 1) { snprintf(buf, n, "%d roots", roots); return -1; }
  return 0;
}

int fkx_open_io(const fk_io *io, const fkx_options *o, fkx **out) {
  fkx_options d;
  fkx_options_default(&d);
  if (!o) o = &d;
  fkx *x = calloc(1, sizeof *x);
  if (!x) return FKX_ENOMEM;
  x->io = *io;
  snprintf(x->vfs_name, sizeof x->vfs_name, "%s", o->vfs_name ? o->vfs_name : "forkdb");
  snprintf(x->root_name, sizeof x->root_name, "%s", o->root_name ? o->root_name : "main");
  x->undo_keep = o->undo_keep > FKX_UNDO_KEEP_MAX ? FKX_UNDO_KEEP_MAX : o->undo_keep;
  x->random64 = o->random64 ? o->random64 : sys_random;
  x->random_ctx = o->random_ctx;
  int rc;
  if (!name_ok(x->root_name)) { rc = failf(x, FKX_EINVAL, "'%s' is not a valid branch name", x->root_name); goto fail; }
  if ((rc = fk_open(&x->io, &x->s)) != FK_OK) { rc = store_fail(x, rc, "opening the store"); goto fail; }
  if (fk_version(x->s) < 4) {
    if (!o->upgrade) { rc = failf(x, FKX_EVERSION, "the store is format version %d: open it with upgrade to compact it into version 4", fk_version(x->s)); goto fail; }
    if ((rc = fk_compact(x->s)) != FK_OK) { rc = store_fail(x, rc, "upgrading the store by compaction"); goto fail; }
  }
  if ((rc = fkvfs_register(x->s, x->vfs_name, &x->vfs)) != 0) { rc = failf(x, FKX_ESQLITE, "registering the VFS '%s' failed (%d)", x->vfs_name, rc); x->sqlite_rc = rc; goto fail; }
  /* adopt a store with nothing in it but branch 0, or validate */
  {
    uint32_t cnt = fk_branch_count(x->s), live = 0;
    for (uint32_t i = 0; i < cnt; i++) live += fk_branch_alive(x->s, i) ? 1u : 0u;
    Meta m;
    if (live == 1 && meta_load(x, 0, &m) == 1) {
      memset(&m, 0, sizeof m);
      m.kind = FKX_ROOT;
      m.nlen = (unsigned)strlen(x->root_name);
      memcpy(m.name, x->root_name, m.nlen + 1);
      m.uid = new_uid(x);
      if ((rc = meta_store(x, 0, &m)) != FKX_OK) goto fail;
    } else {
      for (uint32_t i = 0; i < cnt; i++)
        if (fk_branch_alive(x->s, i) && meta_load(x, i, &m) != 0) { rc = failf(x, FKX_ECORRUPT, "branch %u was not made by fkx: this store is not a managed repository", i); goto fail; }
    }
  }
  fkx_install_guard(x);
  /* what an interrupted operation left behind goes first; what is left must be consistent */
  if ((rc = gc_impl(x, NULL)) != FKX_OK) goto fail;
  {
    char why[200];
    if (fkx_check(x, why, sizeof why) != 0) { rc = failf(x, FKX_ECORRUPT, "the repository is inconsistent: %s", why); goto fail; }
  }
  *out = x;
  return FKX_OK;
fail:
  if (x->vfs) fkvfs_unregister(x->vfs);
  if (x->s) fk_close(x->s);
  free(x);
  return rc;
}

int fkx_open(const char *path, const fkx_options *o, fkx **out) {
  fkx_options d;
  fkx_options_default(&d);
  if (!o) o = &d;
  if (!o->create && access(path, F_OK) != 0) return FKX_ENOENT;
  fk_io io;
  if (fk_file_open(path, &io) != 0) return FKX_ESTORE;
  int wrapped = 0;
  if (o->prealloc_chunk) wrapped = fk_prealloc_wrap(&io, o->prealloc_chunk) == 0;
  fkx *x = NULL;
  int rc = fkx_open_io(&io, o, &x);
  if (rc != FKX_OK) {
    if (wrapped) fk_prealloc_close(&io);
    fk_file_close(&io);
    return rc;
  }
  x->io = io;
  x->own_file = 1;
  x->own_prealloc = wrapped;
  *out = x;
  return FKX_OK;
}

void fkx_close(fkx *x) {
  if (!x) return;
  while (x->trk) { Trk *t = x->trk; x->trk = t->next; void *h = t->h; void (*fn)(void *) = t->fn; free(t); fn(h); } /* a replay or diff handle the host forgot is rolled back */
  fkvfs_unregister(x->vfs);
  fk_close(x->s);
  if (x->own_prealloc) fk_prealloc_close(&x->io);
  if (x->own_file) fk_file_close(&x->io);
  free(x);
}

/* the VFS guard: a branch whose metadata says read-only is not opened for writing */
static int guard(void *ctx, uint32_t branch, int write) {
  fkx *x = ctx;
  Meta m;
  if (!write) return 1;
#ifdef FKX_BUG_GUARD_ALLOWS_WRITES
  return 1; /* mutant: the guard lets everything through */
#endif
  return !(meta_load(x, branch, &m) == 0 && m.ro);
}
static void fkx_install_guard(fkx *x) { fkvfs_set_guard(x->vfs, guard, x); }

/* ---------- branch info ---------- */
static void to_info(const fkx *x, uint32_t id, const Meta *m, fkx_info *o) {
  memset(o, 0, sizeof *o);
  memcpy(o->name, m->name, m->nlen + 1);
  o->kind = m->kind;
  o->read_only = m->ro;
  o->uid = m->uid;
  o->origin_uid = find_uid(x, m->origin, NULL, NULL) ? m->origin : 0;
  o->base_uid = m->base;
  o->owner_uid = m->owner;
  (void)fk_state(x->s, id, &o->state);
  (void)fk_size(x->s, id, &o->size_pages);
  o->n_undo = m->nundo;
  o->open_connections = refs(x, id);
  o->id = id;
}
int fkx_info_get(fkx *x, const char *ref, fkx_info *out) {
  uint32_t id;
  Meta m;
  int rc = resolve(x, ref, &id, &m);
  if (rc) return rc;
  to_info(x, id, &m, out);
  return FKX_OK;
}
int fkx_list(fkx *x, int include_internal, fkx_info *out, size_t cap, size_t *n) {
  size_t cnt = 0;
  uint32_t nb = fk_branch_count(x->s);
  Meta m;
  for (uint32_t i = 0; i < nb; i++) {
    if (!fk_branch_alive(x->s, i) || meta_load(x, i, &m) != 0) continue;
    if (!include_internal && (m.kind == FKX_BASE || m.kind == FKX_UNDO)) continue;
    if (out && cnt < cap) to_info(x, i, &m, &out[cnt]);
    cnt++;
  }
  if (n) *n = cnt;
  return FKX_OK;
}

/* ---------- discard helpers ---------- */
static int discard_id(fkx *x, uint32_t id) {
  int rc = fk_discard(x->s, id);
  return rc == FK_OK ? FKX_OK : store_fail(x, rc, "discarding a branch");
}
/* an undo point's two snapshots, by uid; a missing one is fine */
static int discard_uid(fkx *x, uint64_t uid) {
  uint32_t id;
  if (!find_uid(x, uid, &id, NULL)) return FKX_OK;
  return discard_id(x, id);
}

/* ---------- fork, snapshot, discard, reset ---------- */
static int new_branch_meta(fkx *x, uint32_t src, const Meta *m, uint32_t *id) {
  uint8_t b[FK_META_MAX];
  size_t n = meta_encode(m, b);
  int rc = fk_fork_meta(x->s, src, b, n, id);
  return rc == FK_OK ? FKX_OK : store_fail(x, rc, "forking");
}

int fkx_fork(fkx *x, const char *from, const char *name) {
  uint32_t src, idA, idF;
  Meta sm;
  int rc;
  if (!name_ok(name)) return failf(x, FKX_EINVAL, "'%s' is not a valid branch name (1 to %d of letters, digits and . _ - /, not starting with #)", name ? name : "(null)", FKX_NAME_MAX);
  if ((rc = resolve(x, from, &src, &sm))) return rc;
#ifndef FKX_BUG_NAMES_NOT_UNIQUE
  if (find_name(x, name, NULL, NULL)) return failf(x, FKX_EEXIST, "a branch named '%s' exists", name);
#endif
  if (alive_count(x) + 2 > FK_MAX_BRANCHES) return failf(x, FKX_ELIMIT, "a fork needs two store branches (its base and itself) and %u of %u are in use", alive_count(x), (unsigned)FK_MAX_BRANCHES);
  Meta a, f;
  memset(&a, 0, sizeof a);
  memset(&f, 0, sizeof f);
  a.kind = FKX_BASE; a.ro = 1; a.uid = new_uid(x); a.origin = sm.uid;
  f.kind = FKX_FORK; f.uid = new_uid(x); while (f.uid == a.uid) f.uid = new_uid(x);
  f.origin = sm.uid; f.base = a.uid; f.nlen = (unsigned)strlen(name); memcpy(f.name, name, f.nlen + 1);
  a.owner = f.uid;
#ifdef FKX_BUG_FORK_BEFORE_BASE
  /* mutant: the fork is made first, from the source, and the base after it: a cut leaves a fork with no base */
  if ((rc = new_branch_meta(x, src, &f, &idF))) return rc;
  if ((rc = new_branch_meta(x, src, &a, &idA))) return rc;
  (void)idA;
  return FKX_OK;
#endif
  if ((rc = new_branch_meta(x, src, &a, &idA))) return rc;      /* 1: the base: a read-only copy of the source as it is now */
  if ((rc = new_branch_meta(x, idA, &f, &idF))) return rc;      /* 2: the fork, from the base */
  return FKX_OK;
}

int fkx_snapshot(fkx *x, const char *from, const char *name) {
  uint32_t src, id;
  Meta sm, s;
  int rc;
  if (!name_ok(name)) return failf(x, FKX_EINVAL, "'%s' is not a valid branch name", name ? name : "(null)");
  if ((rc = resolve(x, from, &src, &sm))) return rc;
  if (find_name(x, name, NULL, NULL)) return failf(x, FKX_EEXIST, "a branch named '%s' exists", name);
  if (alive_count(x) + 1 > FK_MAX_BRANCHES) return failf(x, FKX_ELIMIT, "no free store branch (%u of %u in use)", alive_count(x), (unsigned)FK_MAX_BRANCHES);
  memset(&s, 0, sizeof s);
  s.kind = FKX_SNAPSHOT; s.ro = 1; s.uid = new_uid(x); s.origin = sm.uid; s.nlen = (unsigned)strlen(name); memcpy(s.name, name, s.nlen + 1);
  return new_branch_meta(x, src, &s, &id);
}

int fkx_discard(fkx *x, const char *name) {
  uint32_t id, bid;
  Meta m, bm;
  int rc;
  if ((rc = resolve(x, name, &id, &m))) return rc;
  if (m.kind == FKX_ROOT) return failf(x, FKX_EINVAL, "the root branch cannot be discarded");
  if (m.kind != FKX_FORK && m.kind != FKX_SNAPSHOT) return failf(x, FKX_EINVAL, "an internal branch cannot be discarded by hand (fkx_gc removes it when its owner is gone)");
  int has_base = m.kind == FKX_FORK && find_uid(x, m.base, &bid, &bm);
  if (refs(x, id)) return failf(x, FKX_EBUSY, "%u connection(s) are open on '%s'", refs(x, id), name);
  if (has_base && refs(x, bid)) return failf(x, FKX_EBUSY, "a connection is open on the base of '%s'", name);
  for (unsigned e = 0; e < m.nundo; e++)
    for (int k = 0; k < 2; k++) { uint32_t uid; if (find_uid(x, m.undo[e][k], &uid, NULL) && refs(x, uid)) return failf(x, FKX_EBUSY, "a connection is open on an undo snapshot of '%s'", name); }
#ifdef FKX_BUG_DISCARD_BASE_FIRST
  if (has_base && (rc = discard_id(x, bid))) return rc; /* mutant: the base goes first: a cut leaves a live fork with no base */
#endif
  if ((rc = discard_id(x, id))) return rc;                      /* 1: the branch itself, so nothing live points at what goes next */
#ifndef FKX_BUG_DISCARD_BASE_FIRST
#ifndef FKX_BUG_DISCARD_LEAVES_BASE
  if (has_base && (rc = discard_id(x, bid))) return rc;         /* 2: its base */
#endif
#endif
  for (unsigned e = 0; e < m.nundo; e++)                        /* 3: its undo points */
    for (int k = 0; k < 2; k++) if ((rc = discard_uid(x, m.undo[e][k]))) return rc;
  return FKX_OK;
}

int fkx_reset(fkx *x, const char *name, const char *to) {
  uint32_t id, tid;
  Meta m, tm;
  int rc;
  if ((rc = resolve(x, name, &id, &m))) return rc;
  if (m.ro) return failf(x, FKX_EREADONLY, "'%s' is read-only", name);
  if (to) { if ((rc = resolve(x, to, &tid, &tm))) return rc; }
  else {
    if (m.kind != FKX_FORK) return failf(x, FKX_EINVAL, "'%s' has no base to reset to: name the branch to reset to", name);
    if (!find_uid(x, m.base, &tid, &tm)) return failf(x, FKX_ECORRUPT, "the base of '%s' is missing", name);
  }
  if (tid == id) return failf(x, FKX_EINVAL, "a branch cannot be reset to itself");
#ifndef FKX_BUG_RESET_IGNORES_OPEN
  if (refs(x, id)) return failf(x, FKX_EBUSY, "%u connection(s) are open on '%s': close them first (a connection's cache would go stale)", refs(x, id), name);
#endif
  rc = fk_reset(x->s, id, tid);
  return rc == FK_OK ? FKX_OK : store_fail(x, rc, "resetting");
}

/* ---------- open a database on a branch ---------- */
int fkx_open_db(fkx *x, const char *ref, int flags, sqlite3 **db) {
  uint32_t id;
  Meta m;
  char n[24];
  int rc = resolve(x, ref, &id, &m);
  if (rc) return rc;
  if (m.ro && (flags & (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE))) return failf(x, FKX_EREADONLY, "'%s' is read-only: open it with SQLITE_OPEN_READONLY", ref);
  fkvfs_branch_name(id, n, sizeof n);
  rc = sqlite3_open_v2(n, db, flags, x->vfs_name);
  if (rc != SQLITE_OK) {
    x->sqlite_rc = rc;
    int code = failf(x, FKX_ESQLITE, "opening branch '%s' failed: %s", ref, *db ? sqlite3_errmsg(*db) : sqlite3_errstr(rc));
    if (*db) { sqlite3_close(*db); *db = NULL; }
    return code;
  }
  return FKX_OK;
}

/* ---------- undo points, retention, gc ---------- */
/* rewrite the undo list of a branch to its first keep entries; returns the dropped snapshot uids in dropped[] */
static int trim_undo(fkx *x, uint32_t id, Meta *m, unsigned keep, uint64_t (*dropped)[2], unsigned *ndropped) {
  *ndropped = 0;
  if (m->nundo <= keep) return FKX_OK;
  for (unsigned e = keep; e < m->nundo; e++) { dropped[*ndropped][0] = m->undo[e][0]; dropped[*ndropped][1] = m->undo[e][1]; (*ndropped)++; }
  m->nundo = keep;
  return meta_store(x, id, m);
}

static int gc_impl(fkx *x, fkx_gc_report *rep) {
  fkx_gc_report r;
  memset(&r, 0, sizeof r);
  uint32_t nb = fk_branch_count(x->s);
  Meta m;
  int rc;
  /* retention: lower the undo lists to undo_keep (the unlisted snapshots are then collected below) */
  for (uint32_t i = 0; i < nb; i++) {
    if (!fk_branch_alive(x->s, i) || meta_load(x, i, &m) != 0) continue;
    if ((m.kind == FKX_ROOT || m.kind == FKX_FORK) && m.nundo > x->undo_keep) {
      uint64_t d[FKX_UNDO_KEEP_MAX][2];
      unsigned nd = 0;
      if ((rc = trim_undo(x, i, &m, x->undo_keep, d, &nd))) return rc;
      r.over_retention += nd;
    }
  }
  /* orphans */
  uint32_t ids[FK_MAX_BRANCHES], nid = 0;
  int kinds[FK_MAX_BRANCHES];
  for (uint32_t i = 0; i < nb; i++) {
    if (!fk_branch_alive(x->s, i) || meta_load(x, i, &m) != 0) continue;
    if (m.kind != FKX_BASE && m.kind != FKX_UNDO) continue;
    Meta om;
    int owned = find_uid(x, m.owner, NULL, &om);
    int listed = 0;
    if (owned) {
      if (m.kind == FKX_BASE) listed = om.kind == FKX_FORK && om.base == m.uid;
      else if (om.kind == FKX_ROOT || om.kind == FKX_FORK) for (unsigned e = 0; e < om.nundo; e++) listed |= om.undo[e][0] == m.uid || om.undo[e][1] == m.uid;
#ifdef FKX_BUG_GC_TAKES_LISTED_UNDO
      if (m.kind == FKX_UNDO) listed = 0; /* mutant: undo points the owner lists are collected too */
#endif
    }
    if (!owned || !listed) { ids[nid] = i; kinds[nid] = m.kind; nid++; if (!owned && m.kind == FKX_BASE) r.orphan_bases++; else if (!owned) r.orphan_undo++; else if (m.kind == FKX_UNDO) r.unlisted_undo++; else r.orphan_bases++; }
  }
  for (uint32_t k = 0; k < nid; k++) {
    if (!fk_branch_alive(x->s, ids[k]) || refs(x, ids[k])) continue;
    if ((rc = discard_id(x, ids[k]))) return rc;
  }
  (void)kinds;
  if (rep) *rep = r;
  return FKX_OK;
}
int fkx_gc(fkx *x, fkx_gc_report *report) { return gc_impl(x, report); }

int fkx_compact_if_needed(fkx *x, uint64_t min_bytes, int *did) {
  int d = 0;
  int rc = fk_compact_if_needed(x->s, min_bytes, &d);
  if (did) *did = d;
  return rc == FK_OK ? FKX_OK : store_fail(x, rc, "compacting");
}

/* ---------- promote (fast-forward only in this build) and undo ---------- */
static int state_of(fkx *x, uint32_t id, uint64_t *st) { int rc = fk_state(x->s, id, st); return rc == FK_OK ? FKX_OK : store_fail(x, rc, "reading a state id"); }

/* take an undo snapshot of target t and return its uid and id */
static int undo_snapshot(fkx *x, uint32_t t, const Meta *tm, uint64_t *uid, uint32_t *id) {
  Meta u;
  memset(&u, 0, sizeof u);
  u.kind = FKX_UNDO; u.ro = 1; u.uid = new_uid(x); u.owner = tm->uid; u.origin = tm->uid;
  int rc = new_branch_meta(x, t, &u, id);
  if (rc == FKX_OK) *uid = u.uid;
  return rc;
}

/* 1 of a promote: the snapshot of the target as it is (ub = 0 when retention is 0) */
static int undo_before(fkx *x, uint32_t pid, const Meta *pm, uint64_t *ub) {
  uint32_t idb;
  *ub = 0;
  if (!x->undo_keep) return FKX_OK;
  if (alive_count(x) + 2 > FK_MAX_BRANCHES) return failf(x, FKX_ELIMIT, "a promote records an undo point (two store branches) and %u of %u are in use", alive_count(x), (unsigned)FK_MAX_BRANCHES);
  return undo_snapshot(x, pid, pm, ub, &idb);
}

/* 3 and 4 of a promote: the snapshot of the target as the promote left it, then the list in the target's metadata (newest first, oldest dropped) */
static int undo_after(fkx *x, uint32_t pid, const Meta *pm, uint64_t ub, int *recorded) {
  unsigned keep = x->undo_keep;
  uint64_t ua = 0;
  uint32_t ida;
  int rc;
  if (!keep) return FKX_OK;
  if ((rc = undo_snapshot(x, pid, pm, &ua, &ida))) return rc;
  Meta np = *pm;
  uint64_t dropped[FKX_UNDO_KEEP_MAX + 1][2];
  unsigned nd = 0;
  if (np.nundo >= keep) { for (unsigned e = keep - 1; e < np.nundo; e++) { dropped[nd][0] = np.undo[e][0]; dropped[nd][1] = np.undo[e][1]; nd++; } np.nundo = keep - 1; }
  for (unsigned e = np.nundo; e > 0; e--) { np.undo[e][0] = np.undo[e - 1][0]; np.undo[e][1] = np.undo[e - 1][1]; }
  np.undo[0][0] = ub; np.undo[0][1] = ua; np.nundo++;
  if ((rc = meta_store(x, pid, &np))) return rc;
  *recorded = 1;
  for (unsigned i = 0; i < nd; i++) for (int k = 0; k < 2; k++) if ((rc = discard_uid(x, dropped[i][k]))) return rc;
  return FKX_OK;
}

int fkx_promote(fkx *x, const char *name, const fkx_promote_opts *o, fkx_promote_result *out) {
  fkx_promote_result local;
  if (!out) out = &local;
  memset(out, 0, sizeof *out);
  uint32_t fid, pid, bid;
  Meta fm, pm, bm;
  int rc;
  if ((rc = resolve(x, name, &fid, &fm))) return rc;
  if (fm.kind != FKX_FORK) return failf(x, FKX_EINVAL, "'%s' is not a fork: only a fork can be promoted", name);
  const char *into = o ? o->into : NULL;
  if (into) { if ((rc = resolve(x, into, &pid, &pm))) return rc; }
  else {
    if (!find_uid(x, fm.origin, &pid, &pm)) return failf(x, FKX_EINVAL, "the branch '%s' was forked from is gone: name the target", name);
  }
  if (pm.ro) return failf(x, FKX_EREADONLY, "'%s' is read-only and cannot be promoted into", pm.name);
  if (pid == fid) return failf(x, FKX_EINVAL, "a branch cannot be promoted into itself");
  if (!find_uid(x, fm.base, &bid, &bm)) return failf(x, FKX_ECORRUPT, "the base of '%s' is missing", name);
  uint64_t sf, sp, sb;
  if ((rc = state_of(x, fid, &sf)) || (rc = state_of(x, pid, &sp)) || (rc = state_of(x, bid, &sb))) return rc;
  out->dry_run = o ? o->dry_run : 0;
  if (sp == sf) { out->method = FKX_METHOD_UP_TO_DATE; return FKX_OK; }
#ifdef FKX_BUG_FF_WITHOUT_STATE_CHECK
  sp = sb; /* mutant: the target is taken to be where the fork started, whatever it holds */
#endif
  if (sp != sb || refs(x, pid)) {
    /* the target has moved, or a connection is open on it (a reset would leave that connection's cache stale): replay the fork's work in one
     * SQLite transaction on the target, which needs the session driver */
    int rrc = fkx_i_replay_promote(x, fid, &fm, pid, &pm, bid, out);
    if (sp != sb || (rrc != FKX_ENOSESSION && rrc != FKX_EREFUSED)) return rrc;
    char why[256];
    snprintf(why, sizeof why, "%s", x->err);
    out->method = FKX_METHOD_FAST_FORWARD;
    return failf(x, FKX_EBUSY, "%u connection(s) are open on '%s': close them first (a fast-forward resets it, and a connection's cache would go stale; a replay was not possible: %s)", refs(x, pid), pm.name, why);
  }
  out->method = FKX_METHOD_FAST_FORWARD;
  if (out->dry_run) return FKX_OK;
  uint64_t ub = 0;
#ifndef FKX_BUG_RESET_BEFORE_UNDO_SNAPSHOT
  if ((rc = undo_before(x, pid, &pm, &ub))) return rc;                 /* 1: the state before */
#endif
  if ((rc = fk_reset(x->s, pid, fid)) != FK_OK) return store_fail(x, rc, "resetting the target");   /* 2: the promote itself */
#ifdef FKX_BUG_RESET_BEFORE_UNDO_SNAPSHOT
  if ((rc = undo_before(x, pid, &pm, &ub))) return rc;                 /* mutant: the "before" is taken after the reset */
#endif
  return undo_after(x, pid, &pm, ub, &out->undo_recorded);              /* 3 and 4: the state after, and the list */
}

int fkx_undo(fkx *x, const char *name, fkx_promote_result *out) {
  fkx_promote_result local;
  if (!out) out = &local;
  memset(out, 0, sizeof *out);
  uint32_t pid, idb, ida;
  Meta pm, bm, am;
  int rc;
  if ((rc = resolve(x, name, &pid, &pm))) return rc;
  if (pm.kind != FKX_ROOT && pm.kind != FKX_FORK) return failf(x, FKX_EINVAL, "'%s' cannot have promotes to undo", name);
  if (pm.nundo == 0) return failf(x, FKX_ENOENT, "nothing to undo on '%s'", name);
  if (!find_uid(x, pm.undo[0][0], &idb, &bm) || !find_uid(x, pm.undo[0][1], &ida, &am)) return failf(x, FKX_ECORRUPT, "an undo point of '%s' is missing", name);
  uint64_t sp, sbf, saf;
  if ((rc = state_of(x, pid, &sp)) || (rc = state_of(x, idb, &sbf)) || (rc = state_of(x, ida, &saf))) return rc;
  out->method = FKX_METHOD_NONE;
  if (sp == sbf) {
    /* a cut after the reset and before the entry was removed: it is already undone */
  } else {
#ifdef FKX_BUG_UNDO_WITHOUT_STATE_CHECK
    saf = sp; /* mutant: the target is taken to be where the promote left it */
#endif
    if (sp != saf || refs(x, pid)) {
      /* the target has changed since the promote (its later work is kept), or a connection is open on it: undo by replaying the inverse */
      /* A dry run first, so that a conflict or a refusal is reported with nothing changed. Then the entry is removed BEFORE the replay: a cut
       * between the two leaves a promote that was not undone (and no way to undo it), which is safe; the other order would leave a promote that
       * was undone and an entry that can never be applied again (its replay would conflict with what is already there). */
      out->dry_run = 1;
      int rrc = fkx_i_replay_undo(x, pid, &pm, out);
      out->dry_run = 0;
      if (rrc != FKX_OK) {
        if (sp == saf && (rrc == FKX_ENOSESSION || rrc == FKX_EREFUSED)) {
          char why[256];
          snprintf(why, sizeof why, "%s", x->err);
          return failf(x, FKX_EBUSY, "%u connection(s) are open on '%s': close them first (an undo by reset would leave their caches stale; an undo by replay was not possible: %s)", refs(x, pid), name, why);
        }
        return rrc;
      }
      uint64_t r0 = pm.undo[0][0], r1 = pm.undo[0][1];
      Meta rp = pm;
      for (unsigned e = 1; e < rp.nundo; e++) { rp.undo[e - 1][0] = rp.undo[e][0]; rp.undo[e - 1][1] = rp.undo[e][1]; }
      rp.nundo--;
#ifndef FKX_BUG_UNDO_ENTRY_AFTER_REPLAY
      if ((rc = meta_store(x, pid, &rp))) return rc;
#endif
      rrc = fkx_i_replay_undo(x, pid, &pm, out);
      if (rrc != FKX_OK) {
#ifndef FKX_BUG_UNDO_ENTRY_AFTER_REPLAY
        (void)meta_store(x, pid, &pm); /* unexpected after a clean dry run: put the entry back */
#endif
        return rrc;
      }
#ifdef FKX_BUG_UNDO_ENTRY_AFTER_REPLAY
      if ((rc = meta_store(x, pid, &rp))) return rc; /* mutant: the entry is removed after the replay: a cut leaves a promote undone and an entry that can never be applied */
#endif
      if ((rc = discard_uid(x, r0)) || (rc = discard_uid(x, r1))) return rc;
      return FKX_OK;
    } else {
      if ((rc = fk_reset(x->s, pid, idb)) != FK_OK) return store_fail(x, rc, "resetting the target");
      out->method = FKX_METHOD_FAST_FORWARD;
    }
  }
  uint64_t u0 = pm.undo[0][0], u1 = pm.undo[0][1];
  Meta np = pm;
  for (unsigned e = 1; e < np.nundo; e++) { np.undo[e - 1][0] = np.undo[e][0]; np.undo[e - 1][1] = np.undo[e][1]; }
  np.nundo--;
  if ((rc = meta_store(x, pid, &np))) return rc;
  if ((rc = discard_uid(x, u0)) || (rc = discard_uid(x, u1))) return rc;
  return FKX_OK;
}

/* ---------- for the session driver (fkx_session.c) and its stand-in ---------- */
int fkx_i_fail(fkx *x, int code, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(x->err, sizeof x->err, fmt, ap);
  va_end(ap);
  return code;
}
int fkx_i_resolve(fkx *x, const char *ref, uint32_t *id, Meta *m) { return resolve(x, ref, id, m); }
int fkx_i_find_uid(fkx *x, uint64_t uid, uint32_t *id, Meta *m) { return find_uid(x, uid, id, m); }
uint32_t fkx_i_refs(fkx *x, uint32_t id) { return refs(x, id); }
int fkx_i_undo_before(fkx *x, uint32_t pid, const Meta *pm, uint64_t *ub) { return undo_before(x, pid, pm, ub); }
int fkx_i_undo_after(fkx *x, uint32_t pid, const Meta *pm, uint64_t ub, int *recorded) { return undo_after(x, pid, pm, ub, recorded); }
int fkx_i_discard_uid(fkx *x, uint64_t uid) { return discard_uid(x, uid); }
unsigned fkx_i_undo_keep(const fkx *x) { return x->undo_keep; }
void fkx_i_ref(uint64_t uid, char out[24]) { snprintf(out, 24, "#%016llx", (unsigned long long)uid); }
int fkx_i_track(fkx *x, void *h, void (*fn)(void *)) {
  Trk *t = malloc(sizeof *t);
  if (!t) return FKX_ENOMEM;
  t->h = h; t->fn = fn; t->next = x->trk; x->trk = t;
  return FKX_OK;
}
void fkx_i_untrack(fkx *x, void *h) {
  for (Trk **p = &x->trk; *p; p = &(*p)->next)
    if ((*p)->h == h) { Trk *t = *p; *p = t->next; free(t); return; }
}

/* ---------- result structures (shared by the session driver and its stand-in) ---------- */
void fkx_diff_result_free(fkx_diff_result *r) {
  if (!r) return;
  for (size_t i = 0; i < r->ntables; i++) free(r->tables[i].name);
  free(r->tables);
  free(r->changeset);
  memset(r, 0, sizeof *r);
}
void fkx_promote_result_free(fkx_promote_result *r) {
  if (!r) return;
  for (size_t i = 0; i < r->nconflicts; i++) { free(r->conflicts[i].table); free(r->conflicts[i].key); }
  free(r->conflicts);
  free(r->refusal);
  memset(r, 0, sizeof *r);
}
