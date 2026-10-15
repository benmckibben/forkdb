'use strict';
// better-sqlite3 port of test/ext_node_nosync_test.js (the node:sqlite version, itself a port of the Python class H_NoSync). Written by Mika, reviewed by Ulrich.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { died, later, Database, load, one, tmpdir, openBranch, closeStore, runNode, PRELUDE, SO } = require('./bsq_helpers.js');

const LINUX = process.platform === 'linux';
const FSYNC_COUNT_SO = process.env.FK_FSYNC_COUNT_SO;
if (LINUX) {
  assert.ok(FSYNC_COUNT_SO, 'FK_FSYNC_COUNT_SO must be set on Linux: the fsync interposer is not available');
  assert.ok(fs.existsSync(FSYNC_COUNT_SO), 'FK_FSYNC_COUNT_SO=' + FSYNC_COUNT_SO + ' does not exist: build the interposer first');
}

// forkdb_sync_count() counts only what the store itself asks the operating system for, so a process that merely loads the extension is at exactly 0 (the first test checks it).
// It is a constant, not a measurement, so that any single test can be run alone with --test-name-pattern.
const baselines = { loadOnly: 0 };

// Runs code in a child (like runNode) and returns { r, count }: count is the number of durability calls the child made while the code ran, from forkdb_sync_count().
// With interposer=true (Linux only) it also returns the system calls counted from outside: { r, count, external }.
function runCountedWith(t, interposer, code, ...args) {
  const dir = tmpdir(t);
  const file = path.join(dir, 'cnt.txt');
  const ext = path.join(dir, 'ext.txt');
  const wrapped = [
    'const __counter = load();',
    "const __n = () => Number(Object.values(__counter.prepare('select forkdb_sync_count()').get())[0]);",
    'const __n0 = __n();',
    "process.on('exit', (status) => { if (status === 0) require('node:fs').writeFileSync(process.env.FK_SYNC_COUNT_FILE, 'SYNCS ' + (__n() - __n0) + '\\n'); });",
    code,
  ].join('\n');
  const env = { ...process.env, FK_SYNC_COUNT_FILE: file };
  if (interposer) { env.LD_PRELOAD = FSYNC_COUNT_SO; env.FK_FSYNC_COUNT_FILE = ext; }
  const r = spawnSync(process.execPath, ['--no-warnings', '-e', PRELUDE + wrapped, SO, ...args], { encoding: 'utf8', timeout: 120000, env });
  const read = (f, re) => { try { const m = fs.readFileSync(f, 'utf8').match(re); return m ? parseInt(m[1], 10) : -1; } catch (e) { return -1; } };
  return { r, count: read(file, /^SYNCS (\d+)/m), external: interposer ? read(ext, /^FSYNC_COUNT (\d+)/m) : undefined };
}
const runCounted = (t, code, ...args) => runCountedWith(t, false, code, ...args);

// The baseline again, through the counted runner the tests use: it must agree.
test('bsq nosync baseline: loading the extension alone issues 0 syncs', (t) => {
  const { r, count } = runCounted(t, 'load();\n', '/tmp/nonexistent');
  assert.equal(r.status, 0, r.stderr || r.stdout);
  t.diagnostic('baseline (load only) count=' + count);
  assert.equal(count, 0, 'a process that only loads the extension should issue 0 syncs');
});

// ---- subprocess scripts ----
const NOSYNC_INIT_WORK = [
"const opt = process.argv[3], root = process.argv[4];",
"const c = load();",
"if (opt === 'none') c.prepare('select forkdb_init(?, ?)').get(P, root);",
"else c.prepare('select forkdb_init(?, ?, ?)').get(P, root, opt);",
"const db = new Database(one(c, 'select forkdb_open_db(?, ?)', P, root));",
"db.exec('create table t(a integer primary key, b text)');",
"db.exec('create index tb on t(b)');",
"const ins = db.prepare('insert into t values (?, ?)');",
"db.exec('begin');",
"for (let i = 0; i < 100; i++) ins.run(i, 'v' + i);",
"db.exec('commit');",
"for (let i = 0; i < 30; i++) {",
"  c.prepare('select forkdb_fork(?, ?, ?)').get(P, root, 't' + i);",
"  c.prepare('select forkdb_discard(?, ?)').get(P, 't' + i);",
"}",
"c.prepare('select forkdb_close(?)').get(P);",
].join('\n');

const NOSYNC_INIT_WORK_SHORT = [
"const opt = process.argv[3], root = process.argv[4];",
"const c = load();",
"if (opt === 'none') c.prepare('select forkdb_init(?, ?)').get(P, root);",
"else c.prepare('select forkdb_init(?, ?, ?)').get(P, root, opt);",
"c.prepare('select forkdb_close(?)').get(P);",
].join('\n');

const NOSYNC_INIT_WORK_NAMED = [
"const opt = process.argv[3], root = process.argv[4];",
"const c = load();",
"if (opt === 'none') c.prepare('select forkdb_init(?, ?)').get(P, root);",
"else c.prepare('select forkdb_init(?, ?, ?)').get(P, root, opt);",
"const db = new Database(one(c, 'select forkdb_open_db(?, ?)', P, root));",
"db.exec('create table t(a integer primary key)');",
"const ins = db.prepare('insert into t values (?)');",
"db.exec('begin');",
"for (let i = 0; i < 10; i++) ins.run(i);",
"db.exec('commit');",
"c.prepare('select forkdb_close(?)').get(P);",
].join('\n');

const NOSYNC_CRASH = [
"const c = load();",
"c.prepare(\"select forkdb_init(?, 'main', 'nosync')\").get(P);",
"const db = new Database(one(c, \"select forkdb_open_db(?, 'main')\", P));",
"for (let txn = 0; txn < 20; txn++) {",
"  db.exec('create table if not exists t(a integer primary key, b text)');",
"  const cnt = one(db, 'select ifnull(max(a), -1) + 1 from t');",
"  db.exec('begin');",
"  db.prepare(\"insert into t values (?, 'row' || ?)\").run(cnt, cnt);",
"  db.exec('commit');",
"}",
"process.kill(process.pid, 'SIGKILL');",
].join('\n');

const NOSYNC_LOCK = [
"const c = load();",
"try {",
"  one(c, \"select forkdb_open_db(?, 'main')\", P);",
"  console.log('NOT REFUSED');",
"  process.exit(3);",
"} catch (e) {",
"  console.log('error: ' + e.message);",
"  process.exit(0);",
"}",
].join('\n');

const NOSYNC_REOPEN = [
"const c = load();",
"const db = new Database(one(c, \"select forkdb_open_db(?, 'main')\", P));",
"db.exec('begin');",
"db.prepare(\"insert into t values (100, 'durable-write')\").run();",
"db.exec('commit');",
"db.close();",
"c.prepare('select forkdb_close(?)').get(P);",
].join('\n');

const NOSYNC_COMPACT = [
"const opt = process.argv[3], root = process.argv[4];",
"const c = load();",
"if (opt === 'none') c.prepare('select forkdb_init(?, ?)').get(P, root);",
"else c.prepare('select forkdb_init(?, ?, ?)').get(P, root, opt);",
"const db = new Database(one(c, 'select forkdb_open_db(?, ?)', P, root));",
"db.exec('create table t(a integer primary key, b text)');",
"db.exec('create index tb on t(b)');",
"const ins = db.prepare('insert into t values (?, ?)');",
"db.exec('begin');",
"for (let i = 0; i < 2000; i++) ins.run(i, 'v' + i);",
"db.exec('commit');",
"for (let i = 0; i < 20; i++) {",
"  c.prepare('select forkdb_fork(?, ?, ?)').get(P, root, 'f' + i);",
"  const db2 = new Database(one(c, 'select forkdb_open_db(?, ?)', P, 'f' + i));",
"  db2.exec('begin');",
"  db2.exec(\"update t set b = 'x' where a < 100\");",
"  db2.exec('commit');",
"  db2.close();",
"  c.prepare('select forkdb_discard(?, ?)').get(P, 'f' + i);",
"}",
"console.log('compacted ' + one(c, 'select forkdb_compact(?)', P));",
"c.prepare('select forkdb_close(?)').get(P);",
].join('\n');

// ---- tests ----

// 1. A default store (no option) issues fsyncs.
test('bsq nosync default store issues fsyncs', (t) => {
  const dir = tmpdir(t);
  const store = path.join(dir, 'def-syncs.fk');
  const { r, count } = runCounted(t, NOSYNC_INIT_WORK, store, 'none', 'main');
  assert.equal(r.status, 0, r.stderr || r.stdout);
  t.diagnostic('default count=' + count + ', baseline=' + baselines.loadOnly);
  assert.ok(count > (baselines.loadOnly || 0), 'default (durable) store should issue fsyncs, got ' + count);
});

// 2. nosync issues no fsyncs.
test('bsq nosync nosync issues no fsyncs', (t) => {
  const dir = tmpdir(t);
  const store = path.join(dir, 'nosync-none.fk');
  const { r, count } = runCounted(t, NOSYNC_INIT_WORK, store, 'nosync', 'main');
  assert.equal(r.status, 0, r.stderr || r.stdout);
  t.diagnostic('nosync count=' + count + ', baseline=' + baselines.loadOnly);
  assert.equal(count, baselines.loadOnly, 'nosync store should issue exactly ' + baselines.loadOnly + ' fsyncs, got ' + count);
});

// 3. nosync option words with other words and separators.
test('bsq nosync option words with other words and separators', async (t) => {
  const cases = [
    { opt: 'nosync, nosync', root: 'nosync', desc: 'nosync with spaces and comma' },
    { opt: ',nosync', root: 'nosync', desc: 'a leading comma' },
    { opt: '  nosync  ,  ', root: 'nosync', desc: 'runs of separators' },
    { opt: 'nosync,', root: 'nosync', desc: 'nosync with trailing comma' },
    { opt: 'nosync nosync', root: 'nosync', desc: 'two nosync words' },
  ];
  for (const { opt, root, desc } of cases) {
    await t.test(desc, (st) => {
      const dir = tmpdir(st);
      const store = path.join(dir, 'var-' + desc.replace(/\s+/g, '-') + '.fk');
      const { r, count } = runCounted(st, NOSYNC_INIT_WORK_SHORT, store, opt, root);
      assert.equal(r.status, 0, 'failed with opt=' + JSON.stringify(opt) + ' root=' + JSON.stringify(root) + ': ' + (r.stderr || r.stdout) + ' rc=' + r.status);
      assert.equal(count, baselines.loadOnly, 'nosync with opt=' + JSON.stringify(opt) + ' should give ' + baselines.loadOnly + ' fsyncs, got ' + count);
    });
  }
  // Also test with a root name other than 'main'
  const dir2 = tmpdir(t);
  const store2 = path.join(dir2, 'trunk-nosync.fk');
  const { r: r2, count: c2 } = runCounted(t, NOSYNC_INIT_WORK_NAMED, store2, 'nosync', 'trunk');
  assert.equal(r2.status, 0, 'failed with trunk: ' + (r2.stderr || r2.stdout));
  assert.equal(c2, baselines.loadOnly, 'nosync with root trunk should give ' + baselines.loadOnly + ' fsyncs, got ' + c2);
});

// 4. an unknown option word is an error and does not create a store.
test('bsq nosync unknown option is an error', async (t) => {
  const cases = ['nosnyc', 'nosync bogus', 'nosync,bogus', 'nosync, bogus'];
  for (const opt of cases) {
    await t.test('opt: ' + opt, (st) => {
      const dir = tmpdir(st);
      const store = path.join(dir, 'bad.fk');
      const r = runNode(NOSYNC_INIT_WORK_SHORT, [store, opt, 'main']);
      assert.notEqual(r.status, 0, 'expected error for opt=' + JSON.stringify(opt) + ', got status=' + r.status + ' stdout=' + r.stdout + ' stderr=' + r.stderr);
      const combined = r.stdout + r.stderr;
      assert.ok(combined.includes('unknown option'), 'expected "unknown option" in output: ' + combined);
      if (opt.includes('bogus')) assert.ok(combined.includes('bogus'), 'expected "bogus" in output for ' + opt);
      assert.ok(!fs.existsSync(store), 'an unknown option must not create the store file');
    });
  }
});

// 5. nosync data survives process death.
test('bsq nosync data survives process death', (t) => {
  const dir = tmpdir(t);
  const store = path.join(dir, 'crash.fk');
  const r = runNode(NOSYNC_CRASH, [store]);
  assert.ok(died(r), 'the child must have died by the signal: ' + (r.stdout || '') + (r.stderr || ''));
  const c = load(t);
  later(t, () => closeStore(c, store));
  const db = openBranch(t, c, store, 'main');
  const rows = db.prepare('select a from t order by a').all().map(r => r.a);
  assert.equal(rows.length, 20, 'expected 20 rows after crash, got ' + rows.length);
  assert.deepEqual(rows, Array.from({ length: 20 }, (_, i) => i));
  const integ = db.pragma('integrity_check');
  assert.deepEqual(integ, [{ integrity_check: 'ok' }]);
});

// 6. a nosync store reopens durably: after normal close, open without option, write, fsyncs > baseline.
test('bsq nosync store reopens durably after normal close', (t) => {
  const dir = tmpdir(t);
  const store = path.join(dir, 'reopen.fk');

  // First a nosync session: write 30 rows, fork/discard, close.
  const r1 = runNode([
    'const c = load();',
    "c.prepare(\"select forkdb_init(?, 'main', 'nosync')\").get(P);",
    "const db = new Database(one(c, \"select forkdb_open_db(?, 'main')\", P));",
    "db.exec('create table t(a integer primary key, b text)');",
    "const ins = db.prepare('insert into t values (?, ?)');",
    "db.exec('begin');",
    "for (let i = 0; i < 30; i++) ins.run(i, 'nosync-row-' + i);",
    "db.exec('commit');",
    "for (let i = 0; i < 5; i++) {",
    "  c.prepare('select forkdb_fork(?, ?, ?)').get(P, 'main', 'f' + i);",
    "  c.prepare('select forkdb_discard(?, ?)').get(P, 'f' + i);",
    "}",
    "c.prepare('select forkdb_close(?)').get(P);",
    "console.log('nosync session done');",
  ].join('\n'), [store]);
  assert.equal(r1.status, 0, 'nosync session failed: ' + (r1.stderr || r1.stdout));

  // Now a counted subprocess opens it without nosync, does a write.
  const { r: r2, count } = runCounted(t, NOSYNC_REOPEN, store);
  assert.equal(r2.status, 0, 'durable reopen write failed: ' + (r2.stderr || r2.stdout));
  t.diagnostic('reopen count=' + count + ', baseline=' + baselines.loadOnly);
  assert.ok(count > (baselines.loadOnly || 0), 'reopening without nosync should issue fsyncs, got ' + count);

  // Verify data from nosync session is all there.
  const c = load(t);
  later(t, () => closeStore(c, store));
  const db = openBranch(t, c, store, 'main');
  const rows = db.prepare('select a from t where a < 30 order by a').all();
  assert.equal(rows.length, 30, 'expected 30 nosync rows, got ' + rows.length);
  assert.equal(db.prepare('select a from t where a = 100').get().a, 100);
  const integ = db.pragma('integrity_check');
  assert.deepEqual(integ, [{ integrity_check: 'ok' }]);
});

// 7. nosync compaction issues no directory sync.
test('bsq nosync compaction issues no fsyncs and reports compacted 1', (t) => {
  const dir = tmpdir(t);
  const store = path.join(dir, 'nosync-compact.fk');
  const { r, count } = runCounted(t, NOSYNC_COMPACT, store, 'nosync', 'main');
  assert.equal(r.status, 0, 'nosync compaction failed: ' + (r.stderr || r.stdout));
  assert.equal(r.stdout.trim(), 'compacted 1', 'the compaction must really happen: ' + r.stdout);
  t.diagnostic('nosync compact count=' + count + ', baseline=' + baselines.loadOnly);
  assert.equal(count, baselines.loadOnly, 'nosync compaction should issue exactly ' + baselines.loadOnly + ' fsyncs, got ' + count);
});

// 8. a nosync store is still locked: a second process is refused.
test('bsq nosync store is still locked', (t) => {
  const dir = tmpdir(t);
  const store = path.join(dir, 'locked.fk');
  const c = load(t);
  c.prepare("select forkdb_init(?, 'main', 'nosync')").get(store);
  later(t, () => closeStore(c, store));
  const db = openBranch(t, c, store, 'main');
  db.exec('create table t(a)');
  db.exec('begin');
  db.exec('insert into t values (1)');
  db.exec('commit');

  const r = runNode(NOSYNC_LOCK, [store]);
  assert.equal(r.status, 0, 'lock subprocess: ' + (r.stderr || r.stdout));
  assert.ok(r.stdout.includes('open in another process'), 'expected "open in another process": ' + r.stdout);
});

// 9. garbage recipe: nosync compact returns 1 with count==baseline; durable compact > skip.
test('bsq nosync compaction garbage recipe, with and without nosync', (t) => {
  const baserec = [
    "const opt = process.argv[3];",
    "const c = load();",
    "if (opt === 'none') c.prepare('select forkdb_init(?)').get(P);",
    "else c.prepare(\"select forkdb_init(?, 'main', ?)\").get(P, opt);",
    "const db = new Database(one(c, \"select forkdb_open_db(?, 'main')\", P));",
    "db.exec('create table t(a integer primary key, b text)');",
    "const ins = db.prepare('insert into t values (?, ?)');",
    "db.exec('begin');",
    "for (let i = 0; i < 5000; i++) ins.run(i, 'x'.repeat(100));",
    "db.exec('commit');",
    "db.close();",
    "c.prepare(\"select forkdb_fork(?, 'main', 'junk')\").get(P);",
    "const j = new Database(one(c, \"select forkdb_open_db(?, 'junk')\", P));",
    "j.exec('begin');",
    "j.exec(\"update t set b = b || 'abcdefghij'\");",
    "j.exec('commit');",
    "j.close();",
    "c.prepare(\"select forkdb_discard(?, 'junk')\").get(P);",
    "console.log('compacted ' + one(c, 'select forkdb_compact(?)', P));",
    "c.prepare('select forkdb_close(?)').get(P);",
  ].join('\n');

  // nosync run
  const d1 = tmpdir(t);
  const { r: r1, count: c1 } = runCounted(t, baserec, path.join(d1, 'nosync.fk'), 'nosync');
  assert.equal(r1.status, 0, 'nosync garbage compact: ' + (r1.stderr || r1.stdout));
  assert.equal(r1.stdout.trim(), 'compacted 1', 'nosync compaction must return 1: ' + r1.stdout);
  t.diagnostic('nosync garbage compact count=' + c1 + ', baseline=' + baselines.loadOnly);
  assert.equal(c1, baselines.loadOnly, 'nosync garbage compact should have ' + baselines.loadOnly + ' fsyncs, got ' + c1);

  // without nosync run (durable)
  const d2 = tmpdir(t);
  const { r: r2, count: c2 } = runCounted(t, baserec, path.join(d2, 'durable.fk'), 'none');
  assert.equal(r2.status, 0, 'durable garbage compact: ' + (r2.stderr || r2.stdout));
  assert.equal(r2.stdout.trim(), 'compacted 1', 'durable compaction must return 1: ' + r2.stdout);
  t.diagnostic('durable garbage compact count=' + c2 + ', baseline=' + baselines.loadOnly);

  // Without nosync with skip-compact
  const skipBase = [
    "const opt = process.argv[3];",
    "const c = load();",
    "if (opt === 'none') c.prepare('select forkdb_init(?)').get(P);",
    "else c.prepare(\"select forkdb_init(?, 'main', ?)\").get(P, opt);",
    "const db = new Database(one(c, \"select forkdb_open_db(?, 'main')\", P));",
    "db.exec('create table t(a integer primary key, b text)');",
    "const ins = db.prepare('insert into t values (?, ?)');",
    "db.exec('begin');",
    "for (let i = 0; i < 5000; i++) ins.run(i, 'x'.repeat(100));",
    "db.exec('commit');",
    "db.close();",
    "c.prepare(\"select forkdb_fork(?, 'main', 'junk')\").get(P);",
    "const j = new Database(one(c, \"select forkdb_open_db(?, 'junk')\", P));",
    "j.exec('begin');",
    "j.exec(\"update t set b = b || 'abcdefghij'\");",
    "j.exec('commit');",
    "j.close();",
    "c.prepare(\"select forkdb_discard(?, 'junk')\").get(P);",
    "console.log('compacted skipped');",
    "c.prepare('select forkdb_close(?)').get(P);",
  ].join('\n');

  const d3 = tmpdir(t);
  const { r: r3, count: c3 } = runCounted(t, skipBase, path.join(d3, 'skip.fk'), 'none');
  assert.equal(r3.status, 0, 'durable skip compact: ' + (r3.stderr || r3.stdout));
  assert.equal(r3.stdout.trim(), 'compacted skipped');
  t.diagnostic('durable skip compact count=' + c3);

  assert.ok(c2 > c3, 'compaction itself should issue fsyncs on a durable store: compact=' + c2 + ' skip=' + c3);
});

// 10. Linux only: the portable counter agrees with the system calls the interposer sees, for a durable store and for a nosync one.
if (LINUX) {
  test('bsq nosync the portable sync counter agrees with the fsync system calls counted from outside', (t) => {
    for (const opt of ['none', 'nosync']) {
      const dir = tmpdir(t);
      const { r, count, external } = runCountedWith(t, true, NOSYNC_INIT_WORK, path.join(dir, opt + '.fk'), opt, 'main');
      assert.equal(r.status, 0, r.stderr || r.stdout);
      t.diagnostic(opt + ': forkdb_sync_count=' + count + ', fsync and fdatasync calls seen by the interposer=' + external);
      assert.equal(count, external, opt + ': the counter and the interposer must agree');
      if (opt === 'none') assert.ok(count > 0, 'a durable store must have synced'); else assert.equal(count, 0);
    }
  });
}
