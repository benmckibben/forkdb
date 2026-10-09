// forkdb spike: layerfs = in-memory SQLite VFS with copy-on-write page layers (THROWAWAY).
#define _POSIX_C_SOURCE 200809L
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define PS 4096
#define MAXB 8192
#define MAXF 8192

/* ---------- layers ---------- */
typedef struct Layer Layer;
struct Layer { Layer *parent; uint8_t **pg; sqlite3_int64 cap, size; int frozen, depth; long overlay; };
static uint8_t TOMB[1];
static Layer *layer_new(Layer *p){ Layer *l=calloc(1,sizeof *l); l->parent=p; if(p){ l->size=p->size; l->depth=p->depth+1; } return l; }
static void grow(Layer *l, sqlite3_int64 n){
  if(n<=l->cap) return; sqlite3_int64 nc=l->cap?l->cap:64; while(nc<n) nc*=2;
  l->pg=realloc(l->pg,nc*sizeof *l->pg); memset(l->pg+l->cap,0,(nc-l->cap)*sizeof *l->pg); l->cap=nc; }
static const uint8_t *lget(Layer *l, sqlite3_int64 n){
  for(;l;l=l->parent){ if(n<l->cap && l->pg[n]) return l->pg[n]==TOMB?NULL:l->pg[n]; } return NULL; }
static uint8_t *lput(Layer *l, sqlite3_int64 n, int fill){
  grow(l,n+1); uint8_t *p=l->pg[n]; if(p && p!=TOMB) return p;
  int wasTomb=(p==TOMB); const uint8_t *old=(fill && !wasTomb)?lget(l->parent,n):NULL;
  p=malloc(PS); if(old) memcpy(p,old,PS); else memset(p,0,PS);
  l->pg[n]=p; if(!wasTomb) l->overlay++; return p; }
static int layer_read(Layer *l, void *buf, int amt, sqlite3_int64 off){
  uint8_t *o=buf; int got=0; sqlite3_int64 avail=l->size>off?l->size-off:0; int want=avail<amt?(int)avail:amt;
  while(got<want){ sqlite3_int64 pn=(off+got)/PS; int po=(int)((off+got)%PS); int n=PS-po; if(n>want-got) n=want-got;
    const uint8_t *p=lget(l,pn); if(p) memcpy(o+got,p+po,n); else memset(o+got,0,n); got+=n; }
  if(got<amt){ memset(o+got,0,amt-got); return SQLITE_IOERR_SHORT_READ; } return SQLITE_OK; }
static void layer_write(Layer *l, const void *buf, int amt, sqlite3_int64 off){
  const uint8_t *s=buf; int done=0;
  while(done<amt){ sqlite3_int64 pn=(off+done)/PS; int po=(int)((off+done)%PS); int n=PS-po; if(n>amt-done) n=amt-done;
    uint8_t *p=lput(l,pn,1); memcpy(p+po,s+done,n); done+=n; }
  if(off+amt>l->size) l->size=off+amt; }
static void layer_truncate(Layer *l, sqlite3_int64 ns){
  sqlite3_int64 old=l->size; if(ns>=old){ l->size=ns; return; }
  sqlite3_int64 first=(ns+PS-1)/PS, last=(old+PS-1)/PS;
  for(sqlite3_int64 pn=first;pn<last;pn++){ grow(l,pn+1);
    if(l->pg[pn] && l->pg[pn]!=TOMB){ free(l->pg[pn]); l->pg[pn]=TOMB; } else if(!l->pg[pn]){ l->pg[pn]=TOMB; l->overlay++; } }
  l->size=ns; }

/* ---------- branches, shm, memfiles ---------- */
typedef struct Shm { uint8_t *reg[32]; int nreg; int sh[8], ex[8]; } Shm;
typedef struct Branch { char name[96]; int used; Layer *head; int nShared, reserved, pending, exclusive; Shm shm; } Branch;
static Branch BR[MAXB];
static Branch *br_find(const char *n, int create){
  int free_i=-1; for(int i=0;i<MAXB;i++){ if(BR[i].used){ if(!strcmp(BR[i].name,n)) return &BR[i]; } else if(free_i<0) free_i=i; }
  if(!create || free_i<0) return NULL; Branch *b=&BR[free_i]; memset(b,0,sizeof *b); b->used=1; snprintf(b->name,sizeof b->name,"%s",n); b->head=layer_new(NULL); return b; }
typedef struct MemFile { char name[128]; uint8_t *buf; sqlite3_int64 size, cap; int used; } MemFile;
static MemFile MF[MAXF];
static MemFile *mf_find(const char *n){ for(int i=0;i<MAXF;i++) if(MF[i].used && !strcmp(MF[i].name,n)) return &MF[i]; return NULL; }
static MemFile *mf_new(const char *n){ for(int i=0;i<MAXF;i++) if(!MF[i].used){ memset(&MF[i],0,sizeof MF[i]); MF[i].used=1; snprintf(MF[i].name,sizeof MF[i].name,"%s",n); return &MF[i]; } return NULL; }
static void mf_del(MemFile *m){ free(m->buf); memset(m,0,sizeof *m); }
static void mf_write(MemFile *m,const void*b,int amt,sqlite3_int64 off){
  if(off+amt>m->cap){ sqlite3_int64 nc=m->cap?m->cap:4096; while(nc<off+amt) nc*=2; m->buf=realloc(m->buf,nc); memset(m->buf+m->cap,0,nc-m->cap); m->cap=nc; }
  memcpy(m->buf+off,b,amt); if(off+amt>m->size) m->size=off+amt; }

/* fork = O(1): freeze the head, give both sides a fresh empty overlay */
static Branch *br_fork(const char *src, const char *dst){
  Branch *s=br_find(src,0); if(!s) return NULL; Branch *d=br_find(dst,1); if(!d) return NULL;
  Layer *f=s->head;
  if(f->overlay==0 && f->parent && f->size==f->parent->size) f=f->parent; /* empty head: share its parent */
  else { f->frozen=1; s->head=layer_new(f); }
  d->head=layer_new(f); return d; }
static void br_fork_wal(const char *src,const char *dst){
  char a[160],b[160]; snprintf(a,sizeof a,"%s-wal",src); snprintf(b,sizeof b,"%s-wal",dst);
  MemFile *w=mf_find(a); if(!w) return; MemFile *n=mf_new(b); mf_write(n,w->buf,(int)w->size,0); }
static void crash_reset(const char *name){
  Branch *b=br_find(name,0); if(!b) return; b->nShared=b->reserved=b->pending=b->exclusive=0;
  for(int i=0;i<b->shm.nreg;i++) free(b->shm.reg[i]); memset(&b->shm,0,sizeof b->shm); }

/* ---------- fault injection + counters ---------- */
static long io_count, fault_after=-1; static int failed, torn, g_batch, g_failcommit, g_quiet;
static long g_begin,g_commit,g_rollback,g_jopen,g_syncs,g_maxbatch;
static int fault_step(void){ if(failed) return 2; if(fault_after>=0 && io_count>=fault_after){ failed=1; return 1; } io_count++; return 0; }

/* ---------- VFS ---------- */
typedef struct LFile { sqlite3_file base; Branch *b; MemFile *m; int lock, delonclose; Layer *stage; } LFile;
#define CUR(p) ((p)->stage?(p)->stage:(p)->b->head)
static sqlite3_vfs layerfs;
static sqlite3_vfs *dflt(void){ return (sqlite3_vfs*)layerfs.pAppData; }

static void apply_stage(LFile *p){
  Layer *s=p->stage,*h=p->b->head;
  for(sqlite3_int64 n=0;n<s->cap;n++){ uint8_t *q=s->pg[n]; if(!q) continue;
    if(q==TOMB){ grow(h,n+1); if(h->pg[n] && h->pg[n]!=TOMB) free(h->pg[n]); else if(!h->pg[n]) h->overlay++; h->pg[n]=TOMB; }
    else { memcpy(lput(h,n,0),q,PS); free(q); } }
  h->size=s->size; if(s->overlay>g_maxbatch) g_maxbatch=s->overlay; free(s->pg); free(s); p->stage=NULL; }
static void drop_stage(LFile *p){ Layer *s=p->stage; if(!s) return; for(sqlite3_int64 n=0;n<s->cap;n++) if(s->pg[n] && s->pg[n]!=TOMB) free(s->pg[n]); free(s->pg); free(s); p->stage=NULL; }

static int lfUnlock(sqlite3_file *f,int level){
  LFile *p=(LFile*)f; Branch *b=p->b; if(!b || p->lock<=level) return SQLITE_OK;
  if(p->lock>=SQLITE_LOCK_RESERVED){ b->pending=0; if(level<SQLITE_LOCK_RESERVED) b->reserved=0; }
  if(p->lock==SQLITE_LOCK_EXCLUSIVE) b->exclusive=0;
  if(level==SQLITE_LOCK_NONE) b->nShared--;
  p->lock=level; return SQLITE_OK; }
static int lfLock(sqlite3_file *f,int level){
  LFile *p=(LFile*)f; Branch *b=p->b; if(!b || p->lock>=level) return SQLITE_OK;
  if(p->lock==SQLITE_LOCK_NONE){ if(b->pending||b->exclusive) return SQLITE_BUSY; b->nShared++; p->lock=SQLITE_LOCK_SHARED; if(level==SQLITE_LOCK_SHARED) return SQLITE_OK; }
  if(level>=SQLITE_LOCK_RESERVED && p->lock<SQLITE_LOCK_RESERVED){ if(b->reserved) return SQLITE_BUSY; b->reserved=1; p->lock=SQLITE_LOCK_RESERVED; }
  if(level>=SQLITE_LOCK_PENDING && p->lock<SQLITE_LOCK_EXCLUSIVE){ b->pending=1; if(level==SQLITE_LOCK_EXCLUSIVE){ if(b->nShared>1) return SQLITE_BUSY; b->exclusive=1; p->lock=SQLITE_LOCK_EXCLUSIVE; } }
  return SQLITE_OK; }
static int lfCheckReserved(sqlite3_file *f,int *res){ LFile *p=(LFile*)f; *res=p->b?p->b->reserved:0; return SQLITE_OK; }
static int lfClose(sqlite3_file *f){
  LFile *p=(LFile*)f; if(p->b){ lfUnlock(f,SQLITE_LOCK_NONE); drop_stage(p); }
  if(p->m && p->delonclose) mf_del(p->m); return SQLITE_OK; }
static int lfRead(sqlite3_file *f,void *buf,int amt,sqlite3_int64 off){
  LFile *p=(LFile*)f;
  if(p->b) return layer_read(CUR(p),buf,amt,off);
  MemFile *m=p->m; sqlite3_int64 avail=m->size>off?m->size-off:0; int n=avail<amt?(int)avail:amt;
  if(n>0) memcpy(buf,m->buf+off,n); if(n<amt){ memset((uint8_t*)buf+n,0,amt-n); return SQLITE_IOERR_SHORT_READ; } return SQLITE_OK; }
static int lfWrite(sqlite3_file *f,const void *buf,int amt,sqlite3_int64 off){
  LFile *p=(LFile*)f; int fs=fault_step();
  if(fs){ if(fs==1 && torn && amt>1){ if(p->b) layer_write(CUR(p),buf,amt/2,off); else mf_write(p->m,buf,amt/2,off); } return SQLITE_IOERR_WRITE; }
  if(p->b) layer_write(CUR(p),buf,amt,off); else mf_write(p->m,buf,amt,off); return SQLITE_OK; }
static int lfTruncate(sqlite3_file *f,sqlite3_int64 sz){
  LFile *p=(LFile*)f; if(fault_step()) return SQLITE_IOERR_TRUNCATE;
  if(p->b) layer_truncate(CUR(p),sz); else p->m->size=sz<p->m->size?sz:p->m->size; return SQLITE_OK; }
static int lfSync(sqlite3_file *f,int fl){ (void)f;(void)fl; if(fault_step()) return SQLITE_IOERR_FSYNC; g_syncs++; return SQLITE_OK; }
static int lfSize(sqlite3_file *f,sqlite3_int64 *ps){ LFile *p=(LFile*)f; *ps=p->b?CUR(p)->size:p->m->size; return SQLITE_OK; }
static int lfFileControl(sqlite3_file *f,int op,void *arg){
  LFile *p=(LFile*)f; (void)arg;
  switch(op){
    case SQLITE_FCNTL_SIZE_HINT: return SQLITE_OK;
    case SQLITE_FCNTL_BEGIN_ATOMIC_WRITE: if(!p->b) return SQLITE_NOTFOUND; g_begin++; p->stage=layer_new(p->b->head); return SQLITE_OK;
    case SQLITE_FCNTL_COMMIT_ATOMIC_WRITE:
      if(!p->stage) return SQLITE_MISUSE;
      if(g_failcommit || fault_step()){ drop_stage(p); return SQLITE_IOERR; }
      g_commit++; apply_stage(p); return SQLITE_OK;
    case SQLITE_FCNTL_ROLLBACK_ATOMIC_WRITE: g_rollback++; drop_stage(p); return SQLITE_OK;
  } return SQLITE_NOTFOUND; }
static int lfSector(sqlite3_file *f){ (void)f; return PS; }
static int lfDevChar(sqlite3_file *f){ (void)f; return g_batch?SQLITE_IOCAP_BATCH_ATOMIC:0; }
static int lfShmMap(sqlite3_file *f,int iPg,int pgsz,int ext,void volatile **pp){
  LFile *p=(LFile*)f; if(!p->b) return SQLITE_IOERR; Shm *s=&p->b->shm; if(iPg>=32) return SQLITE_NOMEM;
  if(iPg>=s->nreg){ if(!ext){ *pp=0; return SQLITE_OK; } for(int i=s->nreg;i<=iPg;i++) s->reg[i]=calloc(1,pgsz); s->nreg=iPg+1; }
  *pp=s->reg[iPg]; return SQLITE_OK; }
static int lfShmLock(sqlite3_file *f,int ofs,int n,int fl){
  LFile *p=(LFile*)f; Shm *s=&p->b->shm;
  if(fl&SQLITE_SHM_UNLOCK){ for(int i=ofs;i<ofs+n;i++){ if(fl&SQLITE_SHM_SHARED){ if(s->sh[i]>0) s->sh[i]--; } else s->ex[i]=0; } return SQLITE_OK; }
  if(fl&SQLITE_SHM_SHARED){ if(s->ex[ofs]) return SQLITE_BUSY; s->sh[ofs]++; return SQLITE_OK; }
  for(int i=ofs;i<ofs+n;i++) if(s->ex[i]||s->sh[i]) return SQLITE_BUSY;
  for(int i=ofs;i<ofs+n;i++) s->ex[i]=1; return SQLITE_OK; }
static void lfShmBarrier(sqlite3_file *f){ (void)f; __sync_synchronize(); }
static int lfShmUnmap(sqlite3_file *f,int del){
  LFile *p=(LFile*)f; Shm *s=&p->b->shm; if(del){ for(int i=0;i<s->nreg;i++) free(s->reg[i]); memset(s,0,sizeof *s); } return SQLITE_OK; }
static int lfFetch(sqlite3_file *f,sqlite3_int64 o,int a,void **pp){ (void)f;(void)o;(void)a; *pp=0; return SQLITE_OK; }
static int lfUnfetch(sqlite3_file *f,sqlite3_int64 o,void *p){ (void)f;(void)o;(void)p; return SQLITE_OK; }
static const sqlite3_io_methods lfIo = { 3, lfClose, lfRead, lfWrite, lfTruncate, lfSync, lfSize, lfLock, lfUnlock, lfCheckReserved,
  lfFileControl, lfSector, lfDevChar, lfShmMap, lfShmLock, lfShmBarrier, lfShmUnmap, lfFetch, lfUnfetch };

static int lfOpen(sqlite3_vfs *v,const char *zName,sqlite3_file *f,int flags,int *pOut){
  (void)v; LFile *p=(LFile*)f; memset(p,0,sizeof *p); static int tmpn; char tn[64];
  if(!zName){ snprintf(tn,sizeof tn,"tmp-%d",++tmpn); zName=tn; flags|=SQLITE_OPEN_DELETEONCLOSE|SQLITE_OPEN_CREATE; }
  if(flags&SQLITE_OPEN_MAIN_DB){ p->b=br_find(zName,1); if(!p->b) return SQLITE_FULL; }
  else { MemFile *m=mf_find(zName); if(!m){ if(!(flags&SQLITE_OPEN_CREATE)) return SQLITE_CANTOPEN; m=mf_new(zName); if(!m) return SQLITE_FULL; }
    p->m=m; p->delonclose=(flags&SQLITE_OPEN_DELETEONCLOSE)?1:0; if(strstr(zName,"-journal")) g_jopen++; }
  p->base.pMethods=&lfIo; if(pOut) *pOut=flags; return SQLITE_OK; }
static int lfDelete(sqlite3_vfs *v,const char *z,int sync){ (void)v;(void)sync; if(fault_step()) return SQLITE_IOERR_DELETE; MemFile *m=mf_find(z); if(m) mf_del(m); return SQLITE_OK; }
static int lfAccess(sqlite3_vfs *v,const char *z,int fl,int *res){ (void)v; if(fl==SQLITE_ACCESS_EXISTS) *res=(mf_find(z)||br_find(z,0))?1:0; else *res=1; return SQLITE_OK; }
static int lfFull(sqlite3_vfs *v,const char *z,int n,char *out){ (void)v; snprintf(out,n,"%s",z); return SQLITE_OK; }
static int lfRand(sqlite3_vfs *v,int n,char *o){ (void)v; return dflt()->xRandomness(dflt(),n,o); }
static int lfSleep(sqlite3_vfs *v,int us){ (void)v; return dflt()->xSleep(dflt(),us); }
static int lfTime(sqlite3_vfs *v,double *t){ (void)v; return dflt()->xCurrentTime(dflt(),t); }
static int lfTime64(sqlite3_vfs *v,sqlite3_int64 *t){ (void)v; return dflt()->xCurrentTimeInt64(dflt(),t); }
static int lfErr(sqlite3_vfs *v,int n,char *z){ (void)v;(void)n;(void)z; return 0; }

/* ---------- test helpers ---------- */
static int nfail;
#define CHECK(c,msg) do{ int _ok=!!(c); printf("  %s %s\n",_ok?"PASS":"FAIL",msg); if(!_ok) nfail++; }while(0)
static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e9+t.tv_nsec; }
static void X(sqlite3 *db,const char *sql){ char *e=0; int rc=sqlite3_exec(db,sql,0,0,&e); if(rc && !g_quiet) printf("  SQL ERR %d %s: %.80s\n",rc,e?e:"",sql); sqlite3_free(e); }
static sqlite3 *O(const char *name){ sqlite3 *db; int rc=sqlite3_open_v2(name,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,"layerfs"); if(rc){ if(!g_quiet) printf("  open fail %d\n",rc); } return db; }
static long long Q1(sqlite3 *db,const char *sql){ sqlite3_stmt *s; if(sqlite3_prepare_v2(db,sql,-1,&s,0)){ return -1; } long long v=-1; if(sqlite3_step(s)==SQLITE_ROW) v=sqlite3_column_int64(s,0); sqlite3_finalize(s); return v; }
static char *Qs(sqlite3 *db,const char *sql){ sqlite3_stmt *s; if(sqlite3_prepare_v2(db,sql,-1,&s,0)) return strdup("(error)"); char *r=strdup("(null)"); if(sqlite3_step(s)==SQLITE_ROW){ const unsigned char *t=sqlite3_column_text(s,0); free(r); r=strdup(t?(const char*)t:"(null)"); } sqlite3_finalize(s); return r; }
static void resetc(void){ g_begin=g_commit=g_rollback=g_jopen=g_syncs=g_maxbatch=0; }

/* T1: rollback-journal modes */
static void t1(void){
  printf("== T1 rollback-journal modes on the layered VFS\n"); g_batch=0;
  const char *modes[]={"DELETE","TRUNCATE","PERSIST","MEMORY","OFF"};
  for(int i=0;i<5;i++){ char br[32],sql[160]; snprintf(br,sizeof br,"t1_%s",modes[i]); sqlite3 *db=O(br);
    snprintf(sql,sizeof sql,"PRAGMA journal_mode=%s",modes[i]); char *jm=Qs(db,sql);
    X(db,"CREATE TABLE t(id INTEGER PRIMARY KEY,v TEXT)"); X(db,"BEGIN");
    X(db,"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<1000) INSERT INTO t SELECT x,'row'||x FROM c"); X(db,"COMMIT");
    if(strcmp(modes[i],"OFF")){ X(db,"BEGIN"); X(db,"DELETE FROM t"); X(db,"ROLLBACK"); }
    long long n=Q1(db,"SELECT count(*) FROM t"); char *ic=Qs(db,"PRAGMA integrity_check");
    snprintf(sql,sizeof sql,"journal_mode=%-8s (reported %s): rows=%lld integrity=%s%s",modes[i],jm,n,ic,strcmp(modes[i],"OFF")?", rollback restored 1000 rows":"");
    CHECK(n==1000 && !strcmp(ic,"ok"),sql); free(jm); free(ic); sqlite3_close(db); } }

/* T2: batch-atomic */
static void bigtxn(sqlite3 *db,int rows){ char sql[256]; X(db,"BEGIN");
  snprintf(sql,sizeof sql,"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<%d) INSERT INTO t(v) SELECT zeroblob(100) FROM c",rows); X(db,sql); X(db,"COMMIT"); }
static void t2(void){
  printf("== T2 SQLITE_IOCAP_BATCH_ATOMIC on the layered VFS\n"); g_batch=1; sqlite3 *db=O("t2");
  X(db,"PRAGMA journal_mode=DELETE"); X(db,"CREATE TABLE t(id INTEGER PRIMARY KEY,v BLOB)"); resetc(); bigtxn(db,10);
  printf("  small txn (10 rows): begin=%ld commit=%ld rollback=%ld journal-opens=%ld\n",g_begin,g_commit,g_rollback,g_jopen);
  CHECK(g_commit==1 && g_jopen==0,"small txn commits through one atomic batch, no journal file");
  int caches[]={-2000,-256,-64,-16};
  for(int i=0;i<4;i++){ char sql[64]; snprintf(sql,sizeof sql,"PRAGMA cache_size=%d",caches[i]); X(db,sql); resetc(); bigtxn(db,5000);
    long long n=Q1(db,"SELECT count(*) FROM t");
    printf("  5000-row txn, cache_size=%dKB: begin=%ld commit=%ld rollback=%ld journal-opens=%ld max-batch-pages=%ld (rows now %lld)\n",-caches[i],g_begin,g_commit,g_rollback,g_jopen,g_maxbatch,n); }
  long long before=Q1(db,"SELECT count(*) FROM t"); g_failcommit=1; resetc(); g_quiet=1; bigtxn(db,10); g_quiet=0; g_failcommit=0;
  long long after=Q1(db,"SELECT count(*) FROM t"); char *ic=Qs(db,"PRAGMA integrity_check");
  printf("  failing COMMIT_ATOMIC_WRITE: begin=%ld commit=%ld rollback=%ld journal-opens=%ld rows %lld -> %lld integrity=%s\n",g_begin,g_commit,g_rollback,g_jopen,before,after,ic);
  CHECK((after==before||after==before+10) && !strcmp(ic,"ok"),"failed atomic commit leaves old or fully-new state, never partial"); free(ic); sqlite3_close(db); }

/* T3: WAL */
static void t3(void){
  printf("== T3 WAL mode on the layered VFS (xShm* in memory)\n"); g_batch=0; sqlite3 *db=O("t3"); char *jm=Qs(db,"PRAGMA journal_mode=WAL");
  CHECK(!strcmp(jm,"wal"),"journal_mode=WAL accepted"); free(jm);
  X(db,"CREATE TABLE t(id INTEGER PRIMARY KEY,v TEXT)"); for(int i=0;i<300;i++){ char sql[96]; snprintf(sql,sizeof sql,"INSERT INTO t VALUES(%d,'r')",i); X(db,sql); }
  sqlite3 *r=O("t3"); X(db,"BEGIN"); X(db,"INSERT INTO t VALUES(1000,'pending')");
  long long rn=Q1(r,"SELECT count(*) FROM t"); CHECK(rn==300,"second connection reads a consistent snapshot while a write txn is open");
  X(db,"COMMIT"); rn=Q1(r,"SELECT count(*) FROM t"); CHECK(rn==301,"second connection sees the commit");
  MemFile *w=mf_find("t3-wal"); Branch *b=br_find("t3",0);
  printf("  state before fork: db file=%lld bytes, -wal=%lld bytes\n",(long long)b->head->size,w?(long long)w->size:-1LL);
  g_quiet=1; br_fork("t3","t3_nowal"); sqlite3 *f=O("t3_nowal"); long long n=Q1(f,"SELECT count(*) FROM t"); sqlite3_close(f);
  printf("  fork of the db file only (WAL ignored): rows seen=%lld (source has 301)\n",n);
  CHECK(n!=301,"forking only the main file loses un-checkpointed commits (the fork MUST include the WAL or checkpoint first)");
  br_fork("t3","t3_wal"); br_fork_wal("t3","t3_wal"); f=O("t3_wal"); n=Q1(f,"SELECT count(*) FROM t"); char *ic=Qs(f,"PRAGMA integrity_check");
  CHECK(n==301 && !strcmp(ic,"ok"),"fork that carries a copy of the WAL recovers all 301 rows (wal-index rebuilt on open)"); free(ic); sqlite3_close(f);
  X(db,"PRAGMA wal_checkpoint(TRUNCATE)"); w=mf_find("t3-wal"); printf("  after wal_checkpoint(TRUNCATE): -wal=%lld bytes\n",w?(long long)w->size:-1LL);
  br_fork("t3","t3_ckpt"); f=O("t3_ckpt"); n=Q1(f,"SELECT count(*) FROM t"); CHECK(n==301,"checkpoint-then-fork needs no WAL copy (O(1) fork)"); sqlite3_close(f); g_quiet=0;
  sqlite3_close(r); sqlite3_close(db); }

/* T4: O(1) fork, overlay reads, isolation, chain depth */
static void t4(void){
  printf("== T4 copy-on-write layers: fork cost, isolation, chain depth\n"); g_batch=0; int sizes[]={1,16,64};
  for(int k=0;k<3;k++){ int s=sizes[k]; char b[32],sql[200]; snprintf(b,sizeof b,"sz%d",s); sqlite3 *db=O(b);
    X(db,"CREATE TABLE a(id INTEGER PRIMARY KEY,v BLOB)");
    snprintf(sql,sizeof sql,"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<%d) INSERT INTO a SELECT x,randomblob(1000) FROM c",s*1000); X(db,sql); sqlite3_close(db);
    Branch *B=br_find(b,0); double t0=now_ns(); for(int i=0;i<1000;i++){ char nm[48]; snprintf(nm,sizeof nm,"f_%d_%d",s,i); br_fork(b,nm); } double t1=now_ns();
    printf("  db=%dMB (%lld pages): 1000 named forks = %.0f ns each (includes this spike's O(#branches) name lookup), extra memory per fork ~%zu bytes (one empty Layer)\n",s,(long long)(B->head->size/PS),(t1-t0)/1000.0,sizeof(Layer));
    { Layer *keep[1000]; double q0=now_ns(); for(int i=0;i<1000000;i++){ Layer *x=layer_new(B->head); if(i<1000) keep[i]=x; else free(x); } double q1=now_ns(); printf("      core fork op (freeze + allocate one Layer, no name lookup), 1,000,000 runs: %.1f ns each, independent of the %dMB size\n",(q1-q0)/1e6,s); for(int i=0;i<1000;i++) free(keep[i]); } }
  sqlite3 *base=O("sz16"); char *h0=Qs(base,"SELECT hex(substr(v,1,8)) FROM a WHERE id=7"),*h8=Qs(base,"SELECT hex(substr(v,1,8)) FROM a WHERE id=8"); long long cb=Q1(base,"SELECT count(*) FROM a");
  sqlite3 *f=O("f_16_0"); CHECK(Q1(f,"SELECT count(*) FROM a")==cb,"fork reads all base rows through the overlay (empty overlay -> base pages)");
  Branch *FB=br_find("f_16_0",0); long ov0=FB->head->overlay; X(f,"UPDATE a SET v=randomblob(1000) WHERE id=7"); long ov1=FB->head->overlay;
  char *h1=Qs(f,"SELECT hex(substr(v,1,8)) FROM a WHERE id=7"); char *hb=Qs(base,"SELECT hex(substr(v,1,8)) FROM a WHERE id=7");
  printf("  one-row UPDATE on a fork: overlay pages %ld -> %ld of %lld total pages\n",ov0,ov1,(long long)(FB->head->size/PS));
  CHECK(strcmp(h0,h1) && !strcmp(h0,hb),"fork write visible in fork, invisible in base");
  X(base,"UPDATE a SET v=randomblob(1000) WHERE id=8"); char *g8=Qs(f,"SELECT hex(substr(v,1,8)) FROM a WHERE id=8");
  CHECK(!strcmp(g8,h8),"write to base AFTER the fork does not leak into the fork (snapshot semantics)");
  sqlite3_close(f); sqlite3_close(base);
  char prev[48]="sz16"; int depths[]={1,100,400}; int di=0;
  for(int d=1;d<=400;d++){ char nx[48]; snprintf(nx,sizeof nx,"chain_%d",d); br_fork(prev,nx); sqlite3 *c=O(nx); char sql[96]; snprintf(sql,sizeof sql,"UPDATE a SET v=randomblob(1000) WHERE id=%d",d); X(c,sql); sqlite3_close(c); snprintf(prev,sizeof prev,"%s",nx);
    if(d==depths[di]){ sqlite3 *c2=O(nx); double t0=now_ns(); long long sum=Q1(c2,"SELECT sum(length(v)) FROM a"); double t1=now_ns(); printf("  chain depth %3d: full scan of 16MB through %d layers = %.1f ms (sum=%lld)\n",d,br_find(nx,0)->head->depth,(t1-t0)/1e6,sum); sqlite3_close(c2); di++; } } }

/* T5: session extension */
typedef struct { int n; int action; } Conf; static int xConf(void *ctx,int t,sqlite3_changeset_iter *it){ (void)t;(void)it; Conf *c=ctx; c->n++; return c->action; }
static int cs_count(int n,void *p){ sqlite3_changeset_iter *it; sqlite3changeset_start(&it,n,p); int c=0; while(sqlite3changeset_next(it)==SQLITE_ROW) c++; sqlite3changeset_finalize(it); return c; }
static char *cks(sqlite3 *db){ return Qs(db,"SELECT group_concat(id||':'||v) FROM (SELECT * FROM a ORDER BY id)"); }
static void t5(void){
  printf("== T5 session extension: logical diff, promote, conflicts, undo\n"); g_batch=0; sqlite3 *db=O("s5");
  X(db,"CREATE TABLE a(id INTEGER PRIMARY KEY,v TEXT)"); X(db,"CREATE TABLE b(x,y)");
  X(db,"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<100) INSERT INTO a SELECT x,'base'||x FROM c");
  X(db,"INSERT INTO b VALUES(1,1),(2,2),(3,3),(4,4),(5,5)"); sqlite3_close(db);
  br_fork("s5","s5f"); sqlite3 *f=O("s5f"); sqlite3_session *s; sqlite3session_create(f,"main",&s); sqlite3session_attach(s,NULL);
  X(f,"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<10) INSERT INTO a SELECT x+100,'new'||x FROM c");
  X(f,"UPDATE a SET v='u' WHERE id<=5"); X(f,"DELETE FROM a WHERE id BETWEEN 50 AND 52");
  X(f,"INSERT INTO b VALUES(99,99)"); X(f,"CREATE TABLE c(id INTEGER PRIMARY KEY,v)"); X(f,"INSERT INTO c VALUES(1,'x')");
  int n; void *p; sqlite3session_changeset(s,&n,&p); int ops=cs_count(n,p);
  printf("  fork made 18 row changes in table a (10 insert, 5 update, 3 delete), 1 insert into pk-less table b, a CREATE TABLE c + 1 insert\n");
  printf("  changeset: %d bytes, %d operations\n",n,ops);
  Branch *FB=br_find("s5f",0); printf("  page-level delta of the same fork: %ld dirty pages (%ld bytes) vs changeset %d bytes\n",FB->head->overlay,FB->head->overlay*PS,n);
  sqlite3 *par=O("s5"); Conf cf={0,SQLITE_CHANGESET_OMIT}; int rc=sqlite3changeset_apply(par,n,p,0,xConf,&cf);
  char *ca=cks(par),*cb=cks(f); long long cn=Q1(par,"SELECT count(*) FROM sqlite_master WHERE name='c'"); long long bn=Q1(par,"SELECT count(*) FROM b");
  printf("  apply to parent: rc=%d conflicts=%d; table a identical to fork=%s; pk-less table b rows parent=%lld fork=%lld; table c exists in parent=%lld\n",rc,cf.n,strcmp(ca,cb)?"NO":"yes",bn,Q1(f,"SELECT count(*) FROM b"),cn);
  CHECK(rc==SQLITE_OK && !strcmp(ca,cb),"fast-forward promote: applying the fork's changeset to the parent reproduces table a exactly");
  CHECK(ops==18 || ops==19,"changeset holds only the PK-table row changes (pk-less table b is NOT tracked; CREATE TABLE is NOT captured)");
  CHECK(bn==5 && cn==0,"sharp edge confirmed: tables without a PRIMARY KEY and DDL do not travel in changesets");
  sqlite3session_delete(s); sqlite3_free(p); sqlite3_close(f); sqlite3_close(par);
  /* divergence + undo */
  db=O("s6"); X(db,"CREATE TABLE a(id INTEGER PRIMARY KEY,v TEXT)"); X(db,"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<100) INSERT INTO a SELECT x,'base'||x FROM c"); char *base=cks(db); sqlite3_close(db);
  br_fork("s6","s6_1"); br_fork("s6","s6_2"); sqlite3 *f1=O("s6_1"),*f2=O("s6_2");
  sqlite3session_create(f1,"main",&s); sqlite3session_attach(s,"a"); X(f1,"UPDATE a SET v='f1' WHERE id<=5"); sqlite3session_changeset(s,&n,&p); sqlite3session_delete(s);
  X(f2,"UPDATE a SET v='f2' WHERE id BETWEEN 3 AND 8"); X(f2,"INSERT INTO a VALUES(200,'f2')");
  Conf c2={0,SQLITE_CHANGESET_REPLACE}; rc=sqlite3changeset_apply(f2,n,p,0,xConf,&c2);
  char *v3=Qs(f2,"SELECT v FROM a WHERE id=3"),*v7=Qs(f2,"SELECT v FROM a WHERE id=7"),*v1=Qs(f2,"SELECT v FROM a WHERE id=1");
  printf("  diverged merge (f1 changed ids 1-5, f2 changed ids 3-8): rc=%d conflicts=%d; id1=%s id3=%s(REPLACE) id7=%s\n",rc,c2.n,v1,v3,v7);
  CHECK(c2.n==3 && !strcmp(v3,"f1") && !strcmp(v7,"f2") && !strcmp(v1,"f1"),"three-way style merge: overlapping rows reported as conflicts (3), non-overlapping rows merged");
  int n2; void *p2; sqlite3changeset_invert(n,p,&n2,&p2); Conf c3={0,SQLITE_CHANGESET_OMIT}; rc=sqlite3changeset_apply(f1,n2,p2,0,xConf,&c3); char *after=cks(f1);
  CHECK(rc==SQLITE_OK && !strcmp(after,base),"inverted changeset undoes the fork's changes exactly (reset/undo primitive)");
  sqlite3_free(p); sqlite3_free(p2); sqlite3_close(f1); sqlite3_close(f2); }

/* T6: crash / fault injection */
static void t6setup(const char *b,const char *jm){ g_quiet=1; failed=0; fault_after=-1; sqlite3 *db=O(b); char sql[64]; snprintf(sql,sizeof sql,"PRAGMA journal_mode=%s",jm); char *r=Qs(db,sql); free(r);
  X(db,"CREATE TABLE t(id INTEGER PRIMARY KEY,v TEXT)"); X(db,"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<100) INSERT INTO t SELECT x,'old' FROM c"); sqlite3_close(db); }
static void t6run(const char *b){ sqlite3 *db=O(b); X(db,"BEGIN"); X(db,"UPDATE t SET v='new'"); X(db,"INSERT INTO t SELECT id+100,'new' FROM t"); X(db,"COMMIT"); sqlite3_close(db); }
static void t6mode(const char *label,const char *jm,int batch){
  g_batch=batch; char b[64]; snprintf(b,sizeof b,"cr_%s_dry",label); t6setup(b,jm); io_count=0; t6run(b); long total=io_count;
  int bad=0,olds=0,news=0; for(long N=0;N<=total;N++){ snprintf(b,sizeof b,"cr_%s_%ld",label,N); t6setup(b,jm);
    fault_after=N; io_count=0; failed=0; torn=1; t6run(b); fault_after=-1; failed=0; crash_reset(b);
    sqlite3 *db=O(b); char *ic=Qs(db,"PRAGMA integrity_check"); long long cnt=Q1(db,"SELECT count(*) FROM t"),nw=Q1(db,"SELECT count(*) FROM t WHERE v='new'");
    int okold=(cnt==100&&nw==0), oknew=(cnt==200&&nw==200); if(strcmp(ic,"ok")||(!okold&&!oknew)){ bad++; printf("    VIOLATION at fault step %ld: integrity=%s rows=%lld new=%lld\n",N,ic,cnt,nw); } else if(okold) olds++; else news++; free(ic); sqlite3_close(db); }
  g_quiet=0; printf("  %-14s: %ld I/O steps; torn-write+crash injected at every step -> old=%d new=%d violations=%d\n",label,total+1,olds,news,bad);
  char m[96]; snprintf(m,sizeof m,"%s: every crash point recovers to old-or-new state with integrity ok",label); CHECK(bad==0,m); }
static void t6(void){ printf("== T6 crash/fault injection (torn write at step N, then all I/O fails, then reopen)\n");
  t6mode("rollback-DELETE","DELETE",0); t6mode("WAL","WAL",0); t6mode("batch-atomic","DELETE",1); }

int main(void){
  layerfs.iVersion=2; layerfs.szOsFile=sizeof(LFile); layerfs.mxPathname=256; layerfs.zName="layerfs"; layerfs.pAppData=sqlite3_vfs_find(NULL);
  layerfs.xOpen=lfOpen; layerfs.xDelete=lfDelete; layerfs.xAccess=lfAccess; layerfs.xFullPathname=lfFull; layerfs.xRandomness=lfRand; layerfs.xSleep=lfSleep; layerfs.xCurrentTime=lfTime; layerfs.xGetLastError=lfErr; layerfs.xCurrentTimeInt64=lfTime64;
  sqlite3_vfs_register(&layerfs,0);
  printf("SQLite %s  session-ext=%s\n",sqlite3_libversion(),
#ifdef SQLITE_ENABLE_SESSION
  "on"
#else
  "off"
#endif
  );
  t1(); t2(); t3(); t4(); t5(); t6(); printf("\nTOTAL FAILS=%d\n",nfail); return nfail?1:0; }
