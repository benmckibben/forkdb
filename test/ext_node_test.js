'use strict';
// Tests for the forkdb loadable extension, driven through Node's built-in node:sqlite. Run by scripts/test-ext-node.sh, which builds the extension first.
// node:sqlite is statically linked to Node's own SQLite build (not the one the C tests pin), has no GIL, and reads SQL integers as JavaScript numbers.
// Every case that can crash or kill the host runs in a child process and asserts on its exit status.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { Worker } = require('node:worker_threads');
const { later, DatabaseSync, SO, load, one, tmpdir, newStore, openBranch, closeStore, runNode } = require('./node_helpers.js');

const INFO = { session_api: 0, vfs: 'forkdb', version: '0.1.0' };
const infoOf = (c) => Object.fromEntries(c.prepare('select key, value from forkdb_info').all().map((r) => [r.key, r.value]));
const seed = (db, n) => {
  db.exec('create table t(a integer primary key, b text)');
  db.exec('begin');
  const ins = db.prepare('insert into t values (?, ?)');
  for (let i = 0; i < n; i++) ins.run(i, 'x');
  db.exec('commit');
};
const count = (db, where) => one(db, 'select count(*) from t' + (where ? ' where ' + where : ''));
const integrity = (db) => db.prepare('pragma integrity_check').all().map((r) => r.integrity_check);

// ---------------------------------------------------------------- A: loading
test('A host: the versions this run is against', (t) => {
  const c = load(t);
  const v = one(c, 'select sqlite_version()');
  t.diagnostic('node ' + process.version + ', node:sqlite SQLite ' + v);
  assert.match(v, /^3\.\d+\.\d+/);
});
test('A loads with the default entry point', (t) => { assert.deepEqual(infoOf(load(t)), INFO); });
test('A loads with each explicit entry point', (t) => {
  for (const entry of ['sqlite3_forkdb_init', 'sqlite3_extension_init']) {
    const c = new DatabaseSync(':memory:', { allowExtension: true });
    later(t, () => c.close());
    c.loadExtension(SO, entry);
    assert.deepEqual(infoOf(c), INFO, entry);
  }
});
test('A refuses to load without allowExtension, with an error and no crash', (t) => {
  const c = new DatabaseSync(':memory:');
  later(t, () => c.close());
  assert.throws(() => c.loadExtension(SO), { code: 'ERR_INVALID_STATE', message: /extension loading is not allowed/ });
});
test('A a missing library is an error', (t) => {
  const c = new DatabaseSync(':memory:', { allowExtension: true });
  later(t, () => c.close());
  assert.throws(() => c.loadExtension('/nonexistent/forkdb'), { code: 'ERR_LOAD_SQLITE_EXTENSION' });
});
test('A loads twice on one connection', (t) => {
  const c = load(t);
  c.loadExtension(SO);
  assert.deepEqual(infoOf(c), INFO);
});
test('A two connections in one process', (t) => {
  assert.deepEqual(infoOf(load(t)), INFO);
  assert.deepEqual(infoOf(load(t)), INFO);
});
test('A the vfs name matches forkdb_info', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  assert.equal(one(c, 'select forkdb_vfs_name(?)', store), INFO.vfs);
});
test('A the library survives closing the connection that loaded it', (t) => {
  // SQLite dlcloses an extension with the connection that loaded it. The library registers a VFS, so it pins itself: a branch connection opened through
  // that VFS keeps working after the loader is closed, and the library can be loaded again.
  const loader = load(null);
  const store = path.join(tmpdir(t), 's.fk');
  loader.prepare('select forkdb_init(?)').get(store);
  const db = openBranch(t, loader, store, 'main');
  seed(db, 10);
  loader.close();
  const other = load(t); // the store is closed through a connection that is still open
  later(t, () => closeStore(other, store));
  assert.equal(count(db), 10);
  db.exec("insert into t values (100, 'after')");
  assert.equal(count(db), 11);
  assert.deepEqual(infoOf(other), INFO);
});

// ---------------------------------------------------------------- B: functions and what Node does with them
test('B a second init on an open store is refused', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  assert.throws(() => c.prepare('select forkdb_init(?)').get(store), { code: 'ERR_SQLITE_ERROR', message: /store is already open/ });
});
test('B unknown branches and missing stores are errors with a message', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  assert.throws(() => c.prepare("select forkdb_open_db(?, 'zzz')").get(store), { message: /no branch named 'zzz'/ });
  assert.throws(() => c.prepare("select forkdb_open_db('/nonexistent/s.fk', 'main')").get(), { message: /no store at/ });
  assert.throws(() => c.prepare('select forkdb_close(?)').get('/nonexistent/s.fk'), { message: /store not open/ });
});
test('B new ids fit in a JavaScript number (node:sqlite throws above 2^53, after the fork has happened)', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  const ids = [];
  for (let i = 0; i < 30; i++) {
    ids.push(one(c, "select forkdb_fork(?, 'main', ?)", store, 'f' + i));
    ids.push(one(c, "select forkdb_snapshot(?, 'main', ?)", store, 's' + i));
  }
  assert.equal(new Set(ids).size, ids.length, 'distinct');
  for (const u of ids) assert.ok(Number.isSafeInteger(u) && u > 0, 'uid ' + u + ' is not a positive safe integer');
});

// ---------------------------------------------------------------- D: end to end
test('D fork isolation and integrity', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  const m = openBranch(t, c, store, 'main');
  seed(m, 200);
  one(c, "select forkdb_fork(?, 'main', 'a')", store);
  const a = openBranch(t, c, store, 'a');
  assert.equal(count(a), 200);
  a.exec("update t set b = 'A' where a < 50");
  a.exec("insert into t values (1000, 'new')");
  assert.equal(count(a, "b = 'A'"), 50);
  assert.equal(count(m, "b = 'A'"), 0, 'main must not see the fork');
  assert.equal(count(m), 200);
  assert.deepEqual(integrity(a), ['ok']);
  assert.deepEqual(integrity(m), ['ok']);
});
test('D uncommitted work is not persisted', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  const m = openBranch(t, c, store, 'main');
  seed(m, 10);
  m.exec('begin');
  m.exec("insert into t values (500, 'lost')");
  m.close();
  const again = openBranch(t, c, store, 'main');
  assert.equal(count(again), 10);
  assert.deepEqual(integrity(again), ['ok']);
});
test('D a snapshot is frozen, and opens only read-only', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  const m = openBranch(t, c, store, 'main');
  seed(m, 20);
  one(c, "select forkdb_snapshot(?, 'main', 'snap')", store);
  m.exec('delete from t');
  assert.equal(count(m), 0);
  const uri = one(c, "select forkdb_open_db(?, 'snap')", store);
  assert.throws(() => new DatabaseSync(uri), { message: /readonly/ });
  const s = new DatabaseSync(uri, { readOnly: true });
  later(t, () => s.close());
  assert.equal(count(s), 20, 'the snapshot still holds what main held');
  assert.throws(() => s.exec('delete from t'), { message: /readonly/ });
});
test('D data survives forkdb_close and a new process', (t) => {
  const c = load(t);
  const store = path.join(tmpdir(t), 's.fk');
  newStore(t, c, store);
  seed(openBranch(t, c, store, 'main'), 300);
  one(c, "select forkdb_fork(?, 'main', 'a')", store);
  openBranch(t, c, store, 'a').exec('delete from t where a >= 100');
  closeStore(c, store);
  const r = runNode([
    'const c = load();',
    'const out = [];',
    "for (const ref of ['main', 'a']) {",
    '  const d = new DatabaseSync(one(c, "select forkdb_open_db(?, ?)", P, ref));',
    '  out.push(one(d, "select count(*) from t") + " " + one(d, "pragma integrity_check"));',
    '  d.close();',
    '}',
    "console.log(out.join('|'));",
  ].join('\n'), store);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.equal(r.stdout.trim(), '300 ok|100 ok');
});
test('D a process killed (SIGKILL) after a commit leaves the data, and the lock is released', (t) => {
  const store = path.join(tmpdir(t), 's.fk');
  const r = runNode([
    'const c = load();',
    'c.prepare("select forkdb_init(?)").get(P);',
    'const d = new DatabaseSync(one(c, "select forkdb_open_db(?, ?)", P, "main"));',
    'd.exec("create table t(a integer primary key, b text)");',
    'd.exec("begin");',
    'for (let i = 0; i < 50; i++) d.exec("insert into t values (" + i + ", \'x\')");',
    'd.exec("commit");',
    'd.exec("begin");',
    'd.exec("insert into t values (999, \'uncommitted\')");',
    'process.kill(process.pid, "SIGKILL");',
  ].join('\n'), store);
  assert.equal(r.signal, 'SIGKILL', 'the child must have died by the signal: ' + r.stdout + r.stderr);
  const c = load(t);
  const db = openBranch(t, c, store, 'main');
  later(t, () => closeStore(c, store));
  assert.equal(count(db), 50, 'the 50 committed rows, and not the uncommitted one');
  assert.deepEqual(integrity(db), ['ok']);
});

// ---------------------------------------------------------------- E: one process at a time
test('E a second process is refused while this one has the store open', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  const m = openBranch(t, c, store, 'main');
  seed(m, 3);
  const r = runNode([
    'const c = load();',
    'try { one(c, "select forkdb_open_db(?, ?)", P, "main"); console.log("NOT REFUSED"); process.exit(3); }',
    'catch (e) { console.log("refused: " + e.message); }',
  ].join('\n'), store);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /open in another process/);
  assert.equal(count(m), 3);
  assert.deepEqual(integrity(m), ['ok']);
});
test('E the lock is released by forkdb_close', (t) => {
  const c = load(t);
  const store = path.join(tmpdir(t), 's.fk');
  newStore(t, c, store);
  seed(openBranch(t, c, store, 'main'), 5);
  closeStore(c, store);
  const r = runNode([
    'const c = load();',
    'const d = new DatabaseSync(one(c, "select forkdb_open_db(?, ?)", P, "main"));',
    'console.log("rows " + one(d, "select count(*) from t"));',
  ].join('\n'), store);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /rows 5/);
});
test('E the lock is released when the holder dies', (t) => {
  const store = path.join(tmpdir(t), 's.fk');
  const r = runNode('const c = load(); c.prepare("select forkdb_init(?)").get(P); process.kill(process.pid, "SIGKILL");', store);
  assert.equal(r.signal, 'SIGKILL', r.stdout + r.stderr);
  const c = load(t);
  const db = openBranch(t, c, store, 'main');
  later(t, () => closeStore(c, store));
  db.exec('create table x(b)');
  db.exec('insert into x values (42)');
  assert.equal(one(db, 'select b from x'), 42);
});

// ---------------------------------------------------------------- F: real threads (worker_threads: no GIL, the connections run in parallel)
const worker = (store, ref, i, iters) => new Promise((resolve, reject) => {
  const w = new Worker(path.join(__dirname, 'ext_node_worker.js'), { workerData: { so: SO, store, ref, iters, base: 100000 * (i + 1) } });
  w.once('message', resolve);
  w.once('error', reject);
  w.once('exit', (code) => { if (code !== 0) reject(new Error('worker exited with ' + code)); });
});
const threadStore = (t, refs) => {
  const c = load(t);
  const store = path.join(tmpdir(t), 's.fk');
  newStore(t, c, store);
  seed(openBranch(t, c, store, 'main'), 200);
  for (const n of ['a', 'b', 'c']) one(c, "select forkdb_fork(?, 'main', ?)", store, n);
  return { c, store };
};
const settle = async (t, c, store, refs, results, iters) => {
  results.forEach((r, i) => { assert.deepEqual(r.errors, [], 'worker ' + i + ' (' + refs[i] + ')'); assert.equal(r.ok, iters, 'worker ' + i + ' acknowledged commits'); });
  const written = {};
  refs.forEach((r, i) => { written[r] = (written[r] || 0) + results[i].ok; });
  const verify = () => {
    for (const ref of Object.keys(written)) {
      const db = openBranch(t, c, store, ref);
      assert.equal(count(db), 200 + written[ref], ref + ' has every acknowledged row, and nothing else');
      assert.deepEqual(integrity(db), ['ok'], ref);
      db.close();
    }
  };
  verify();
  closeStore(c, store);
  verify();
};
test('F writers on separate branches, one worker thread each', async (t) => {
  const { c, store } = threadStore(t);
  const refs = ['main', 'a', 'b', 'c'];
  const results = await Promise.all(refs.map((r, i) => worker(store, r, i, 120)));
  await settle(t, c, store, refs, results, 120);
});
test('F writers on the same branch, one worker thread each', async (t) => {
  const { c, store } = threadStore(t);
  const refs = ['a', 'a', 'a'];
  const results = await Promise.all(refs.map((r, i) => worker(store, r, i, 100)));
  await settle(t, c, store, refs, results, 100);
});
test('F branch management on this thread while workers write', async (t) => {
  const { c, store } = threadStore(t);
  const refs = ['a', 'b', 'c'];
  const pending = refs.map((r, i) => worker(store, r, i, 100));
  for (let k = 0; k < 10; k++) {
    one(c, "select forkdb_fork(?, 'main', 'tmp')", store);
    one(c, "select forkdb_snapshot(?, 'main', 'tmp-snap')", store);
    one(c, "select forkdb_discard(?, 'tmp')", store);
    one(c, "select forkdb_discard(?, 'tmp-snap')", store);
  }
  const results = await Promise.all(pending);
  await settle(t, c, store, refs, results, 100);
});
