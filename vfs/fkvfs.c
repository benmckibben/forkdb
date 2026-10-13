/* fkvfs: see fkvfs.h. */
#include "fkvfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PG FK_PAGE
#define EMPTY 0xffffffffu
#define TOMB 0xfffffffeu

/* ---- staging: the pages a connection has written and not yet published (read-your-writes), a hash table by page number ---- */
typedef struct {
  uint32_t *k;
  uint8_t **v;
  size_t cap, n, used; /* n live, used = live + tombstones; cap is a power of two or 0 */
} Stage;

static size_t hsh(uint32_t k, size_t cap) { return (size_t)(((uint64_t)k * 0x9E3779B97F4A7C15ull) >> 24) & (cap - 1); }

static int stage_grow(Stage *t) {
  size_t ncap = 64;
  while (ncap < (t->n + 1) * 4) ncap *= 2;
  uint32_t *nk = malloc(ncap * sizeof *nk);
  uint8_t **nv = malloc(ncap * sizeof *nv);
  if (!nk || !nv) { free(nk); free(nv); return -1; }
  for (size_t i = 0; i < ncap; i++) { nk[i] = EMPTY; nv[i] = NULL; }
  for (size_t i = 0; i < t->cap; i++) {
    if (t->k[i] == EMPTY || t->k[i] == TOMB) continue;
    size_t j = hsh(t->k[i], ncap);
    while (nk[j] != EMPTY) j = (j + 1) & (ncap - 1);
    nk[j] = t->k[i];
    nv[j] = t->v[i];
  }
  free(t->k);
  free(t->v);
  t->k = nk; t->v = nv; t->cap = ncap; t->used = t->n;
  return 0;
}

static uint8_t *stage_find(const Stage *t, uint32_t k) {
  if (!t->cap) return NULL;
  size_t j = hsh(k, t->cap);
  while (t->k[j] != EMPTY) {
    if (t->k[j] == k) return t->v[j];
    j = (j + 1) & (t->cap - 1);
  }
  return NULL;
}

/* 1 when k is in the table. stage_take clears the value and leaves the key, so this stays true after the buffer has been handed over. */
static int stage_has(const Stage *t, uint32_t k) {
  if (!t->cap) return 0;
  size_t j = hsh(k, t->cap);
  while (t->k[j] != EMPTY) {
    if (t->k[j] == k) return 1;
    j = (j + 1) & (t->cap - 1);
  }
  return 0;
}

/* The page buffer for pgno, allocating it (contents undefined) when new. NULL on out of memory. */
static uint8_t *stage_put(Stage *t, uint32_t k) {
  uint8_t *p = stage_find(t, k);
  if (p) return p;
  if (!t->cap || (t->used + 1) * 2 > t->cap) { if (stage_grow(t)) return NULL; }
  p = malloc(PG);
  if (!p) return NULL;
  size_t j = hsh(k, t->cap);
  while (t->k[j] != EMPTY) j = (j + 1) & (t->cap - 1);
  t->k[j] = k; t->v[j] = p; t->n++; t->used++;
  return p;
}

/* Take the buffer for k out of the table (the key stays, so counts and probing are unchanged); the caller owns it now. */
static uint8_t *stage_take(Stage *t, uint32_t k) {
  if (!t->cap) return NULL;
  size_t j = hsh(k, t->cap);
  while (t->k[j] != EMPTY) {
    if (t->k[j] == k) { uint8_t *p = t->v[j]; t->v[j] = NULL; return p; }
    j = (j + 1) & (t->cap - 1);
  }
  return NULL;
}

static void stage_drop_from(Stage *t, uint32_t limit) { /* forget every page numbered >= limit */
  for (size_t i = 0; i < t->cap; i++) {
    if (t->k[i] != EMPTY && t->k[i] != TOMB && t->k[i] >= limit) { free(t->v[i]); t->v[i] = NULL; t->k[i] = TOMB; t->n--; }
  }
}

static void stage_clear(Stage *t) {
  for (size_t i = 0; i < t->cap; i++) if (t->k[i] != EMPTY && t->k[i] != TOMB) free(t->v[i]);
  free(t->k);
  free(t->v);
  memset(t, 0, sizeof *t);
}

static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}

/* ---- the VFS ---- */
typedef struct Branch { /* in-process lock state of one branch, shared by the connections open on it */
  uint32_t id;
  int nref, nshared, reserved, pending, excl;
  struct Branch *next;
} Branch;

typedef struct MemFile { /* a journal or temporary file: bytes in memory */
  char *name; /* NULL for an anonymous temporary file */
  uint8_t *data;
  size_t len, cap;
  int ref; /* one for the registry while listed, one per open handle */
  int listed;
  struct MemFile *next;
} MemFile;

struct FkVfs {
  fkvfs_guard_fn guard;
  void *guard_ctx;
  sqlite3_vfs vfs;
  sqlite3_vfs *base;
  fk_store *s;
  Branch *branches;
  MemFile *files;
  FkVfsStats st;
  int batch, fail_commits, fail_page, nopen;
};

typedef struct {
  sqlite3_file base;
  FkVfs *v;
  Branch *br;
  uint32_t branch;
  int lock, owns_res, owns_pend;
  Stage st;
  int have_size;       /* the file size has been changed by this connection and not published */
  uint32_t size_pages; /* ... to this */
  uint32_t limit;      /* pages numbered >= limit read as zeros, whatever the store holds (after a truncate) */
} FkFile;

typedef struct {
  sqlite3_file base;
  FkVfs *v;
  MemFile *mf;
  int delete_on_close;
} MemHandle;

static const sqlite3_io_methods fkf_methods, mem_methods;

/* "b12" or "/b12" */
static int parse_branch(const char *z, uint32_t *b) {
  if (!z) return -1;
  if (*z == '/') z++;
  if (*z != 'b') return -1;
  z++;
  if (!*z) return -1;
  uint64_t n = 0;
  for (; *z; z++) {
    if (*z < '0' || *z > '9') return -1;
    n = n * 10 + (uint64_t)(*z - '0');
    if (n >= FK_MAX_BRANCHES) return -1;
  }
  *b = (uint32_t)n;
  return 0;
}

void fkvfs_branch_name(uint32_t branch, char *buf, size_t n) { snprintf(buf, n, "b%u", branch); }

static int map_store_err(int rc) {
  if (rc == FK_ENOMEM) return SQLITE_NOMEM;
  return SQLITE_IOERR_WRITE;
}

static int dirty(const FkFile *f) { return f->st.n > 0 || f->have_size; }

static uint32_t vis_size(FkFile *f) {
  if (f->have_size) return f->size_pages;
  uint32_t p = 0;
  if (fk_size(f->v->s, f->branch, &p) != FK_OK) return 0;
  return p;
}

static void discard(FkFile *f) {
  stage_clear(&f->st);
  f->have_size = 0;
  f->size_pages = 0;
  f->limit = EMPTY;
}

/* Hand everything staged to the store as one transaction. With dedupe, pages equal to the committed ones are dropped first, and a
 * publish with nothing left to say commits nothing (this is what makes a rolled-back transaction that spilled pages cost no write). */
static int publish(FkFile *f, int dedupe) {
  FkVfs *v = f->v;
  fk_store *s = v->s;
  uint32_t cs = 0;
  if (fk_size(s, f->branch, &cs) != FK_OK) return SQLITE_IOERR_WRITE;
  uint32_t ns = f->have_size ? f->size_pages : cs;
  uint32_t *keys = malloc(sizeof(uint32_t) * (f->st.n + 1));
  if (!keys) return SQLITE_NOMEM;
  uint32_t n = 0;
  for (size_t i = 0; i < f->st.cap; i++) if (f->st.k[i] != EMPTY && f->st.k[i] != TOMB) keys[n++] = f->st.k[i];
  qsort(keys, n, sizeof *keys, cmp_u32);
  uint32_t nw = n;
  if (dedupe) {
    uint8_t cur[PG];
    nw = 0;
    for (uint32_t i = 0; i < n; i++) {
      uint32_t pg = keys[i];
      if (pg < cs && pg < f->limit && pg < ns && fk_read(s, f->branch, pg, cur) == FK_OK && memcmp(cur, stage_find(&f->st, pg), PG) == 0) {
        keys[i] = EMPTY;
        v->st.pages_deduped++;
      } else {
        nw++;
      }
    }
  }
  uint32_t zhi = ns < cs ? ns : cs; /* pages the store still has inside the new size but a truncate emptied: write them as zeros */
  uint32_t zlo = f->limit < zhi ? f->limit : zhi;
  int rc = SQLITE_OK;
  if (nw == 0 && zlo == zhi && ns == cs) goto done; /* nothing to commit; done still drops the staging dedup left in the hash */
  int e = fk_txn_begin(s, f->branch);
  if (e != FK_OK) { rc = map_store_err(e); goto done; }
  uint32_t wrote = 0;
  for (uint32_t i = 0; i < n && rc == SQLITE_OK; i++) {
    if (keys[i] == EMPTY) continue;
    /* The store takes the staged buffer itself: a transaction's pages are held once, not twice, until the commit. stage_take
     * clears the value and leaves the key, so the page is still recorded; after any failure below the staging is dropped, not kept
     * for a retry. */
    uint8_t *pg = stage_take(&f->st, keys[i]);
    if (v->fail_page > 0 && wrote + 1 == (uint32_t)v->fail_page) { v->fail_page = 0; e = FK_ENOMEM; } /* test hook */
    else e = pg ? fk_txn_write_owned(s, keys[i], pg) : FK_EINVAL;
    if (e != FK_OK) { free(pg); rc = map_store_err(e); } else wrote++;
  }
  if (rc == SQLITE_OK && zlo < zhi) {
    uint8_t zero[PG];
    memset(zero, 0, sizeof zero);
    for (uint32_t pg = zlo; pg < zhi && rc == SQLITE_OK; pg++) {
      /* Written above: stage_take cleared the buffer and left the key, which is what records the page. */
      if (stage_has(&f->st, pg)) continue;
      e = fk_txn_write(s, pg, zero);
      if (e != FK_OK) rc = map_store_err(e); else wrote++;
    }
  }
  if (rc == SQLITE_OK) {
    e = fk_txn_commit(s, ns);
    if (e != FK_OK) rc = map_store_err(e);
  }
  if (rc != SQLITE_OK) { fk_txn_abort(s); discard(f); goto done; }
  v->st.store_commits++;
  v->st.pages_published += wrote;
done:
  /* A commit and the no-op both drop staging. Dedup only marks the local keys array, so on the no-op path the hash is still
   * occupied (and have_size may still be set): dirty() stays true and cannot be the signal to clear it. */
  if (rc == SQLITE_OK) discard(f);
  free(keys);
  return rc;
}

/* A page of the connection's view: staged, else the store's, else zeros. */
static int page_get(FkFile *f, uint32_t pg, uint8_t *out) {
  uint8_t *p = stage_find(&f->st, pg);
  if (p) { memcpy(out, p, PG); return 0; }
  uint32_t cs = 0;
  if (fk_size(f->v->s, f->branch, &cs) != FK_OK) return -1;
  if (pg >= f->limit || pg >= cs) { memset(out, 0, PG); return 0; }
  return fk_read(f->v->s, f->branch, pg, out) == FK_OK ? 0 : -1;
}

static int unlock_to(FkFile *f, int lv);

static int fkf_close(sqlite3_file *pf) {
  FkFile *f = (FkFile *)pf;
  FkVfs *v = f->v;
  if (dirty(f)) v->st.discarded++;
  discard(f);
  unlock_to(f, SQLITE_LOCK_NONE); /* nothing is staged now, so this publishes nothing */
  Branch *b = f->br;
  if (--b->nref == 0) {
    for (Branch **pp = &v->branches; *pp; pp = &(*pp)->next) if (*pp == b) { *pp = b->next; break; }
    free(b);
  }
  v->nopen--;
  return SQLITE_OK;
}

static int fkf_read_m(sqlite3_file *pf, void *buf, int amt, sqlite3_int64 off) {
  FkFile *f = (FkFile *)pf;
  uint8_t *dst = buf;
  uint8_t tmp[PG];
  uint64_t vsz = (uint64_t)vis_size(f) * PG;
  int got = 0;
  while (got < amt) {
    uint64_t o = (uint64_t)off + (uint64_t)got;
    if (o >= vsz) { memset(dst + got, 0, (size_t)(amt - got)); return SQLITE_IOERR_SHORT_READ; }
    uint32_t pg = (uint32_t)(o / PG);
    int po = (int)(o % PG), n = (int)PG - po;
    if (n > amt - got) n = amt - got;
    if (page_get(f, pg, tmp) != 0) return SQLITE_IOERR_READ;
    memcpy(dst + got, tmp + po, (size_t)n);
    got += n;
  }
  return SQLITE_OK;
}

static int fkf_write_m(sqlite3_file *pf, const void *buf, int amt, sqlite3_int64 off) {
  FkFile *f = (FkFile *)pf;
  if (off < 0 || (off % PG) != 0 || (amt % PG) != 0) return SQLITE_IOERR_WRITE; /* page size must be a multiple of 4096 */
  const uint8_t *src = buf;
  uint32_t cur = vis_size(f);
  if (!f->have_size) { f->have_size = 1; f->size_pages = cur; }
  for (int done = 0; done < amt; done += (int)PG) {
    uint32_t pg = (uint32_t)((uint64_t)(off + done) / PG);
    if (pg >= FK_MAX_PAGES) return SQLITE_FULL;
    uint8_t *p = stage_put(&f->st, pg);
    if (!p) return SQLITE_NOMEM;
    memcpy(p, src + done, PG);
    if (pg + 1 > f->size_pages) f->size_pages = pg + 1;
  }
  return SQLITE_OK;
}

static int fkf_truncate(sqlite3_file *pf, sqlite3_int64 size) {
  FkFile *f = (FkFile *)pf;
  if (size < 0 || (size % PG) != 0) return SQLITE_IOERR_TRUNCATE;
  uint32_t pages = (uint32_t)((uint64_t)size / PG);
  uint32_t cur = vis_size(f);
  if (!f->have_size) { f->have_size = 1; f->size_pages = cur; }
  if (pages < f->size_pages) {
    stage_drop_from(&f->st, pages);
    if (pages < f->limit) f->limit = pages;
  }
  f->size_pages = pages;
  return SQLITE_OK;
}

static int fkf_sync(sqlite3_file *pf, int flags) {
  (void)flags;
  FkFile *f = (FkFile *)pf;
  if (!dirty(f)) return SQLITE_OK;
  int rc = publish(f, 1);
  if (rc == SQLITE_OK) f->v->st.sync_publish++; else f->v->st.sync_publish_errors++;
  return rc;
}

static int fkf_filesize(sqlite3_file *pf, sqlite3_int64 *out) {
  *out = (sqlite3_int64)vis_size((FkFile *)pf) * PG;
  return SQLITE_OK;
}

static int fkf_lock(sqlite3_file *pf, int lv) {
  FkFile *f = (FkFile *)pf;
  Branch *b = f->br;
  if (f->lock >= lv) return SQLITE_OK;
  if (lv == SQLITE_LOCK_SHARED) {
    if (b->pending || b->excl) return SQLITE_BUSY;
    b->nshared++;
    f->lock = lv;
    return SQLITE_OK;
  }
  if (lv == SQLITE_LOCK_RESERVED) {
    if (b->reserved) return SQLITE_BUSY;
    b->reserved = 1;
    f->owns_res = 1;
    f->lock = lv;
    return SQLITE_OK;
  }
  /* PENDING or EXCLUSIVE: become the only reader */
  if (!f->owns_res && b->reserved) return SQLITE_BUSY; /* another connection is writing */
  if (!f->owns_pend) {
    if (b->pending) return SQLITE_BUSY;
    b->pending = 1;
    f->owns_pend = 1;
    f->lock = SQLITE_LOCK_PENDING;
  }
  if (b->nshared > 1) return SQLITE_BUSY; /* readers remain; PENDING stays so no new ones arrive */
  b->excl = 1;
  f->lock = SQLITE_LOCK_EXCLUSIVE;
  return SQLITE_OK;
}

static int unlock_to(FkFile *f, int lv) {
  Branch *b = f->br;
  if (f->lock <= lv) return SQLITE_OK;
  int rc = SQLITE_OK;
  if (lv < SQLITE_LOCK_RESERVED && dirty(f)) {
    /* The write lock is going away with pages staged: this is how a commit under synchronous=OFF (no xSync) is published, and
     * how a rollback that spilled pages ends (equal pages are dropped, so nothing is written). SQLite cannot be told about a
     * failure here; the store is then fail-stop and the next operation reports it. */
    f->v->st.unlock_publish++;
    rc = publish(f, 1);
    if (rc != SQLITE_OK) { f->v->st.unlock_publish_errors++; discard(f); }
  }
  if (f->lock == SQLITE_LOCK_EXCLUSIVE && lv < SQLITE_LOCK_EXCLUSIVE) b->excl = 0;
  if (lv < SQLITE_LOCK_RESERVED && f->owns_res) { b->reserved = 0; f->owns_res = 0; }
  if (lv < SQLITE_LOCK_PENDING && f->owns_pend) { b->pending = 0; f->owns_pend = 0; }
  if (lv < SQLITE_LOCK_SHARED) b->nshared--;
  f->lock = lv;
  (void)rc;
  return SQLITE_OK;
}

static int fkf_unlock(sqlite3_file *pf, int lv) { return unlock_to((FkFile *)pf, lv); }

static int fkf_checkres(sqlite3_file *pf, int *out) {
  Branch *b = ((FkFile *)pf)->br;
  *out = b->reserved || b->pending || b->excl;
  return SQLITE_OK;
}

static int fkf_fcntl(sqlite3_file *pf, int op, void *arg) {
  FkFile *f = (FkFile *)pf;
  FkVfs *v = f->v;
  switch (op) {
    case SQLITE_FCNTL_PRAGMA: {
      /* arg is char *a[]: a[0] result or error message, a[1] the pragma, a[2] its value (NULL when it is only being read).
       * Three settings would put a commit at risk, and are refused here, before SQLite acts on them:
       *  - journal_mode=WAL: SQLite writes the WAL version into the database header and answers "wal" before it opens the -wal file,
       *    so refusing the file in xOpen is too late (in exclusive locking mode WAL needs no shared memory). The branch would be left
       *    claiming WAL while the VFS refuses WAL.
       *  - synchronous=OFF: there is then no xSync, so a commit on the journal path is published when the write lock is dropped, and
       *    unlock has no way to report a failure. A transaction SQLite called committed could be lost.
       *  - journal_mode=MEMORY and OFF: with either, SQLite creates no super-journal, so the refusal in xOpen that keeps a transaction
       *    from writing several attached branches (they would commit one after the other) would not fire. The journal is already
       *    kept in memory here, so these modes buy nothing. */
      char **a = arg;
      if (!a || !a[1] || !a[2]) return SQLITE_NOTFOUND;
      if (sqlite3_stricmp(a[1], "journal_mode") == 0 && sqlite3_stricmp(a[2], "wal") == 0) {
        a[0] = sqlite3_mprintf("forkdb: WAL is not supported");
        return SQLITE_ERROR;
      }
      if (sqlite3_stricmp(a[1], "journal_mode") == 0 && (sqlite3_stricmp(a[2], "memory") == 0 || sqlite3_stricmp(a[2], "off") == 0)) {
        a[0] = sqlite3_mprintf("forkdb: journal_mode=%s is not supported (a transaction must write only one branch, and in this mode SQLite cannot tell the VFS)", a[2]);
        return SQLITE_ERROR;
      }
      if (sqlite3_stricmp(a[1], "synchronous") == 0 && (sqlite3_stricmp(a[2], "off") == 0 || strcmp(a[2], "0") == 0 || sqlite3_stricmp(a[2], "no") == 0 || sqlite3_stricmp(a[2], "false") == 0)) {
        a[0] = sqlite3_mprintf("forkdb: synchronous=OFF is not supported (a commit must be durable)");
        return SQLITE_ERROR;
      }
      return SQLITE_NOTFOUND;
    }
    case SQLITE_FCNTL_BEGIN_ATOMIC_WRITE:
      v->st.atomic_begin++;
      if (dirty(f)) { v->st.atomic_refused++; return SQLITE_IOERR; } /* SQLite then uses the journal */
      return SQLITE_OK;
    case SQLITE_FCNTL_COMMIT_ATOMIC_WRITE: {
      if (v->fail_commits > 0) { v->fail_commits--; v->st.commit_failed++; return SQLITE_IOERR_COMMIT_ATOMIC; }
      int rc = publish(f, 0);
      if (rc != SQLITE_OK) { v->st.commit_failed++; return SQLITE_IOERR_COMMIT_ATOMIC; }
      v->st.atomic_commit++;
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_ROLLBACK_ATOMIC_WRITE:
      v->st.atomic_rollback++;
      discard(f);
      return SQLITE_OK;
    default:
      return SQLITE_NOTFOUND;
  }
}

static int fkf_sector(sqlite3_file *pf) { (void)pf; return (int)PG; }

static int fkf_devchar(sqlite3_file *pf) {
  FkFile *f = (FkFile *)pf;
  int c = SQLITE_IOCAP_POWERSAFE_OVERWRITE;
  if (f->v->batch) c |= SQLITE_IOCAP_BATCH_ATOMIC;
  return c;
}

static const sqlite3_io_methods fkf_methods = {
  1, fkf_close, fkf_read_m, fkf_write_m, fkf_truncate, fkf_sync, fkf_filesize, fkf_lock, fkf_unlock, fkf_checkres, fkf_fcntl, fkf_sector, fkf_devchar,
  0, 0, 0, 0, 0, 0};

/* ---- in-memory files (journals, temporary files) ---- */
static void memfile_unref(MemFile *m) {
  if (--m->ref > 0) return;
  free(m->name);
  free(m->data);
  free(m);
}

static int mem_close(sqlite3_file *pf) {
  MemHandle *h = (MemHandle *)pf;
  if (h->delete_on_close && h->mf->listed) {
    for (MemFile **pp = &h->v->files; *pp; pp = &(*pp)->next) if (*pp == h->mf) { *pp = h->mf->next; break; }
    h->mf->listed = 0;
    memfile_unref(h->mf);
  }
  memfile_unref(h->mf);
  h->v->nopen--;
  return SQLITE_OK;
}

static int mem_read(sqlite3_file *pf, void *buf, int amt, sqlite3_int64 off) {
  MemFile *m = ((MemHandle *)pf)->mf;
  if (off < 0) return SQLITE_IOERR_READ;
  if ((uint64_t)off >= m->len) { memset(buf, 0, (size_t)amt); return SQLITE_IOERR_SHORT_READ; }
  size_t avail = m->len - (size_t)off;
  size_t n = avail < (size_t)amt ? avail : (size_t)amt;
  memcpy(buf, m->data + off, n);
  if (n < (size_t)amt) { memset((uint8_t *)buf + n, 0, (size_t)amt - n); return SQLITE_IOERR_SHORT_READ; }
  return SQLITE_OK;
}

static int mem_reserve(MemFile *m, size_t need) {
  if (need <= m->cap) return 0;
  size_t nc = m->cap ? m->cap : 4096;
  while (nc < need) nc *= 2;
  uint8_t *d = realloc(m->data, nc);
  if (!d) return -1;
  m->data = d;
  m->cap = nc;
  return 0;
}

static int mem_write(sqlite3_file *pf, const void *buf, int amt, sqlite3_int64 off) {
  MemFile *m = ((MemHandle *)pf)->mf;
  if (off < 0) return SQLITE_IOERR_WRITE;
  size_t end = (size_t)off + (size_t)amt;
  if (mem_reserve(m, end)) return SQLITE_NOMEM;
  if ((size_t)off > m->len) memset(m->data + m->len, 0, (size_t)off - m->len);
  memcpy(m->data + off, buf, (size_t)amt);
  if (end > m->len) m->len = end;
  return SQLITE_OK;
}

static int mem_truncate(sqlite3_file *pf, sqlite3_int64 size) {
  MemFile *m = ((MemHandle *)pf)->mf;
  if (size < 0) return SQLITE_IOERR_TRUNCATE;
  if ((size_t)size > m->len) {
    if (mem_reserve(m, (size_t)size)) return SQLITE_NOMEM;
    memset(m->data + m->len, 0, (size_t)size - m->len);
  }
  m->len = (size_t)size;
  return SQLITE_OK;
}

static int mem_sync(sqlite3_file *pf, int flags) { (void)pf; (void)flags; return SQLITE_OK; }
static int mem_size(sqlite3_file *pf, sqlite3_int64 *out) { *out = (sqlite3_int64)((MemHandle *)pf)->mf->len; return SQLITE_OK; }
static int mem_lock(sqlite3_file *pf, int lv) { (void)pf; (void)lv; return SQLITE_OK; }
static int mem_checkres(sqlite3_file *pf, int *out) { (void)pf; *out = 0; return SQLITE_OK; }
static int mem_fcntl(sqlite3_file *pf, int op, void *arg) { (void)pf; (void)op; (void)arg; return SQLITE_NOTFOUND; }
static int mem_sector(sqlite3_file *pf) { (void)pf; return 512; }
static int mem_devchar(sqlite3_file *pf) { (void)pf; return SQLITE_IOCAP_POWERSAFE_OVERWRITE; }

static const sqlite3_io_methods mem_methods = {
  1, mem_close, mem_read, mem_write, mem_truncate, mem_sync, mem_size, mem_lock, mem_lock, mem_checkres, mem_fcntl, mem_sector, mem_devchar,
  0, 0, 0, 0, 0, 0};

static MemFile *memfile_find(FkVfs *v, const char *name) {
  for (MemFile *m = v->files; m; m = m->next) if (m->name && strcmp(m->name, name) == 0) return m;
  return NULL;
}

/* ---- sqlite3_vfs ---- */
static int vfs_open(sqlite3_vfs *pv, const char *zName, sqlite3_file *pf, int flags, int *pOut) {
  FkVfs *v = pv->pAppData;
  if (pOut) *pOut = flags;
  if (flags & SQLITE_OPEN_SUPER_JOURNAL) {
    /* SQLite creates a super-journal when one transaction writes more than one database file (ATTACHed branches). Each branch would
     * then be published to the store by its own commit, one after the other, so a power cut between them leaves one branch changed
     * and the other not. Refuse it: the COMMIT fails with SQLITE_CANTOPEN before anything is published and SQLite rolls the
     * transaction back. The contract is one written branch per transaction (docs/SESSION-DESIGN.md). */
    v->st.super_journal_refused++;
    sqlite3_log(SQLITE_CANTOPEN, "fkvfs: a transaction may write only one branch; this one wrote several attached branches, so it was rolled back (reading several is fine)");
    return SQLITE_CANTOPEN;
  }
  if (flags & SQLITE_OPEN_MAIN_DB) {
    uint32_t b;
    if (parse_branch(zName, &b) != 0 || !fk_branch_alive(v->s, b)) return SQLITE_CANTOPEN;
    if (v->guard && !v->guard(v->guard_ctx, b, (flags & SQLITE_OPEN_READWRITE) != 0)) {
      sqlite3_log(SQLITE_READONLY, "fkvfs: branch %u may not be opened for writing", (unsigned)b);
      return SQLITE_READONLY; /* the layer above says this branch is read-only */
    }
    FkFile *f = (FkFile *)pf;
    memset(f, 0, sizeof *f);
    Branch *br = v->branches;
    while (br && br->id != b) br = br->next;
    if (!br) {
      br = calloc(1, sizeof *br);
      if (!br) return SQLITE_NOMEM;
      br->id = b;
      br->next = v->branches;
      v->branches = br;
    }
    br->nref++;
    f->v = v;
    f->br = br;
    f->branch = b;
    f->limit = EMPTY;
    f->base.pMethods = &fkf_methods;
    v->nopen++;
    return SQLITE_OK;
  }
  if (flags & SQLITE_OPEN_WAL) return SQLITE_CANTOPEN; /* WAL would live in memory and a commit would not be durable */
  MemHandle *h = (MemHandle *)pf;
  memset(h, 0, sizeof *h);
  MemFile *m = zName ? memfile_find(v, zName) : NULL;
  if (m) {
    if ((flags & SQLITE_OPEN_EXCLUSIVE) && (flags & SQLITE_OPEN_CREATE)) return SQLITE_CANTOPEN;
  } else {
    if (zName && !(flags & SQLITE_OPEN_CREATE)) return SQLITE_CANTOPEN;
    m = calloc(1, sizeof *m);
    if (!m) return SQLITE_NOMEM;
    if (zName) {
      m->name = strdup(zName);
      if (!m->name) { free(m); return SQLITE_NOMEM; }
      m->listed = 1;
      m->ref = 1;
      m->next = v->files;
      v->files = m;
    }
  }
  if (flags & SQLITE_OPEN_MAIN_JOURNAL) v->st.journal_opens++;
  m->ref++;
  h->v = v;
  h->mf = m;
  h->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
  h->base.pMethods = &mem_methods;
  v->nopen++;
  return SQLITE_OK;
}

static int vfs_delete(sqlite3_vfs *pv, const char *zName, int syncDir) {
  FkVfs *v = pv->pAppData;
  (void)syncDir;
  MemFile *m = memfile_find(v, zName);
  if (!m) return SQLITE_IOERR_DELETE_NOENT;
  for (MemFile **pp = &v->files; *pp; pp = &(*pp)->next) if (*pp == m) { *pp = m->next; break; }
  m->listed = 0;
  memfile_unref(m);
  return SQLITE_OK;
}

static int vfs_access(sqlite3_vfs *pv, const char *zName, int flags, int *pRes) {
  FkVfs *v = pv->pAppData;
  (void)flags;
  uint32_t b;
  if (parse_branch(zName, &b) == 0) {
    uint32_t pages = 0;
    *pRes = fk_branch_alive(v->s, b) && fk_size(v->s, b, &pages) == FK_OK && (flags != SQLITE_ACCESS_EXISTS || pages > 0);
    return SQLITE_OK;
  }
  *pRes = memfile_find(v, zName) != NULL;
  return SQLITE_OK;
}

static int vfs_fullpath(sqlite3_vfs *pv, const char *zName, int nOut, char *zOut) {
  (void)pv;
  size_t n = strlen(zName);
  if (n >= (size_t)nOut) return SQLITE_CANTOPEN;
  memcpy(zOut, zName, n + 1);
  return SQLITE_OK;
}

static int vfs_random(sqlite3_vfs *pv, int n, char *z) { FkVfs *v = pv->pAppData; return v->base->xRandomness(v->base, n, z); }
static int vfs_sleep(sqlite3_vfs *pv, int us) { FkVfs *v = pv->pAppData; return v->base->xSleep(v->base, us); }
static int vfs_curtime(sqlite3_vfs *pv, double *p) { FkVfs *v = pv->pAppData; return v->base->xCurrentTime(v->base, p); }
static int vfs_lasterr(sqlite3_vfs *pv, int n, char *z) { (void)pv; (void)n; (void)z; return 0; }
static int vfs_curtime64(sqlite3_vfs *pv, sqlite3_int64 *p) {
  FkVfs *v = pv->pAppData;
  if (v->base->iVersion >= 2 && v->base->xCurrentTimeInt64) return v->base->xCurrentTimeInt64(v->base, p);
  double d = 0;
  int rc = v->base->xCurrentTime(v->base, &d);
  *p = (sqlite3_int64)(d * 86400000.0);
  return rc;
}

int fkvfs_register(fk_store *s, const char *name, FkVfs **out) {
  FkVfs *v = calloc(1, sizeof *v);
  if (!v) return SQLITE_NOMEM;
  v->base = sqlite3_vfs_find(NULL);
  if (!v->base) { free(v); return SQLITE_ERROR; }
  v->s = s;
  v->batch = 1;
  char *nm = strdup(name);
  if (!nm) { free(v); return SQLITE_NOMEM; }
  size_t fsz = sizeof(FkFile) > sizeof(MemHandle) ? sizeof(FkFile) : sizeof(MemHandle);
  v->vfs.iVersion = 2;
  v->vfs.szOsFile = (int)fsz;
  v->vfs.mxPathname = 256;
  v->vfs.zName = nm;
  v->vfs.pAppData = v;
  v->vfs.xOpen = vfs_open;
  v->vfs.xDelete = vfs_delete;
  v->vfs.xAccess = vfs_access;
  v->vfs.xFullPathname = vfs_fullpath;
  v->vfs.xRandomness = vfs_random;
  v->vfs.xSleep = vfs_sleep;
  v->vfs.xCurrentTime = vfs_curtime;
  v->vfs.xGetLastError = vfs_lasterr;
  v->vfs.xCurrentTimeInt64 = vfs_curtime64;
  int rc = sqlite3_vfs_register(&v->vfs, 0);
  if (rc != SQLITE_OK) { free(nm); free(v); return rc; }
  *out = v;
  return SQLITE_OK;
}

int fkvfs_unregister(FkVfs *v) {
  if (v->nopen != 0) return SQLITE_BUSY;
  sqlite3_vfs_unregister(&v->vfs);
  while (v->files) { MemFile *m = v->files; v->files = m->next; m->listed = 0; memfile_unref(m); }
  free((void *)v->vfs.zName);
  free(v);
  return SQLITE_OK;
}

void fkvfs_set_store(FkVfs *v, fk_store *s) { v->s = s; }
void fkvfs_stats(const FkVfs *v, FkVfsStats *out) { *out = v->st; }
uint32_t fkvfs_branch_refs(const FkVfs *v, uint32_t branch) {
  for (const Branch *b = v->branches; b; b = b->next)
    if (b->id == branch) return (uint32_t)b->nref;
  return 0;
}
void fkvfs_set_guard(FkVfs *v, fkvfs_guard_fn fn, void *ctx) {
  v->guard = fn;
  v->guard_ctx = ctx;
}
void fkvfs_set_batch_atomic(FkVfs *v, int on) { v->batch = on; }
void fkvfs_fail_commits(FkVfs *v, int n) { v->fail_commits = n; }
void fkvfs_fail_publish_page(FkVfs *v, int n) { v->fail_page = n; }
