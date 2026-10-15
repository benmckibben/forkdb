'use strict';
// better-sqlite3 port of groups B, D, E of test/ext_node_test.js (the node:sqlite suite).
// Group A is in ext_bsq_test.js; group F (worker threads) is in ext_bsq_threads_test.js. Written by Mika, reviewed by Ulrich.
const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const { died, later, Database, load, one, tmpdir, newStore, openBranch, closeStore, runNode } = require('./bsq_helpers.js');

const seed = (db, n) => {
  db.exec('create table t(a integer primary key, b text)');
  db.exec('begin');
  const ins = db.prepare('insert into t values (?, ?)');
  for (let i = 0; i < n; i++) ins.run(i, 'x');
  db.exec('commit');
};
const count = (db, where) => one(db, 'select count(*) from t' + (where ? ' where ' + where : ''));
const integrity = (db) => db.pragma('integrity_check').map((r) => r.integrity_check);

// ---------------------------------------------------------------- B: functions and what Node does with them
test('bsq B a second init on an open store is refused', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  assert.throws(() => c.prepare('select forkdb_init(?)').get(store), { name: 'SqliteError', message: /store is already open/ });
});
test('bsq B unknown branches and missing stores are errors with a message', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  assert.throws(() => c.prepare("select forkdb_open_db(?, 'zzz')").get(store), { message: /no branch named 'zzz'/ });
  assert.throws(() => c.prepare("select forkdb_open_db('/nonexistent/s.fk', 'main')").get(), { message: /no store at/ });
  assert.throws(() => c.prepare('select forkdb_close(?)').get('/nonexistent/s.fk'), { message: /store not open/ });
});
test('bsq B new ids fit in a JavaScript number (better-sqlite3 reads SQL integers as JS numbers)', (t) => {
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
test('bsq D fork isolation and integrity', (t) => {
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
test('bsq D uncommitted work is not persisted', (t) => {
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
test('bsq D a snapshot is frozen, and opens only read-only', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  const m = openBranch(t, c, store, 'main');
  seed(m, 20);
  one(c, "select forkdb_snapshot(?, 'main', 'snap')", store);
  m.exec('delete from t');
  assert.equal(count(m), 0);
  const uri = one(c, "select forkdb_open_db(?, 'snap')", store);
  // better-sqlite3: a read-write open of a snapshot refuses at new Database() with SQLITE_READONLY
  assert.throws(() => new Database(uri), { name: 'SqliteError', message: /read ?only|READONLY/i });
  const s = new Database(uri, { readonly: true });
  later(t, () => s.close());
  assert.equal(count(s), 20, 'the snapshot still holds what main held');
  assert.throws(() => s.exec('delete from t'), { name: 'SqliteError', message: /read ?only|READONLY/i });
});
test('bsq D data survives forkdb_close and a new process', (t) => {
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
    '  const d = new Database(one(c, "select forkdb_open_db(?, ?)", P, ref));',
    '  out.push(one(d, "select count(*) from t") + " " + one(d, "pragma integrity_check"));',
    '  d.close();',
    '}',
    "console.log(out.join('|'));",
  ].join('\n'), [store]);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.equal(r.stdout.trim(), '300 ok|100 ok');
});
test('bsq D a process killed (SIGKILL) after a commit leaves the data, and the lock is released', (t) => {
  const store = path.join(tmpdir(t), 's.fk');
  const r = runNode([
    'const c = load();',
    'c.prepare("select forkdb_init(?)").get(P);',
    'const d = new Database(one(c, "select forkdb_open_db(?, ?)", P, "main"));',
    'd.exec("create table t(a integer primary key, b text)");',
    'd.exec("begin");',
    'for (let i = 0; i < 50; i++) d.exec("insert into t values (" + i + ", \'x\')");',
    'd.exec("commit");',
    'd.exec("begin");',
    'd.exec("insert into t values (999, \'uncommitted\')");',
    'process.kill(process.pid, "SIGKILL");',
  ].join('\n'), [store]);
  assert.ok(died(r), 'the child must have died by the signal: ' + r.stdout + r.stderr);
  const c = load(t);
  const db = openBranch(t, c, store, 'main');
  later(t, () => closeStore(c, store));
  assert.equal(count(db), 50, 'the 50 committed rows, and not the uncommitted one');
  assert.deepEqual(integrity(db), ['ok']);
});

// ---------------------------------------------------------------- E: one process at a time
test('bsq E a second process is refused while this one has the store open', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  const m = openBranch(t, c, store, 'main');
  seed(m, 3);
  const r = runNode([
    'const c = load();',
    'try { one(c, "select forkdb_open_db(?, ?)", P, "main"); console.log("NOT REFUSED"); process.exit(3); }',
    'catch (e) { console.log("refused: " + e.message); }',
  ].join('\n'), [store]);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /open in another process/);
  assert.equal(count(m), 3);
  assert.deepEqual(integrity(m), ['ok']);
});
test('bsq E the lock is released by forkdb_close', (t) => {
  const c = load(t);
  const store = path.join(tmpdir(t), 's.fk');
  newStore(t, c, store);
  seed(openBranch(t, c, store, 'main'), 5);
  closeStore(c, store);
  const r = runNode([
    'const c = load();',
    'const d = new Database(one(c, "select forkdb_open_db(?, ?)", P, "main"));',
    'console.log("rows " + one(d, "select count(*) from t"));',
  ].join('\n'), [store]);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /rows 5/);
});
test('bsq E the lock is released when the holder dies', (t) => {
  const store = path.join(tmpdir(t), 's.fk');
  const r = runNode('const c = load(); c.prepare("select forkdb_init(?)").get(P); process.kill(process.pid, "SIGKILL");', [store]);
  assert.ok(died(r), r.stdout + r.stderr);
  const c = load(t);
  const db = openBranch(t, c, store, 'main');
  later(t, () => closeStore(c, store));
  db.exec('create table x(b)');
  db.exec('insert into x values (42)');
  assert.equal(one(db, 'select b from x'), 42);
});
