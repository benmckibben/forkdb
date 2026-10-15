'use strict';
// better-sqlite3 against the forkdb loadable extension: loading, and the URI hazard. The rest of the suite is in ext_bsq_port_test.js and ext_bsq_nosync_test.js.
// better-sqlite3 bundles its own SQLite, is built with SQLITE_USE_URI=0, and loads extensions with db.loadExtension(path) (no allowExtension flag).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { later, Database, SO, load, one, tmpdir, newStore, openBranch, runNode } = require('./bsq_helpers.js');

const INFO = { session_api: 0, vfs: 'forkdb', version: '0.1.0' };
const infoOf = (c) => Object.fromEntries(c.prepare('select key, value from forkdb_info').all().map((r) => [r.key, r.value]));

test('A host: the versions this run is against', (t) => {
  const c = load(t);
  const v = one(c, 'select sqlite_version()');
  t.diagnostic('node ' + process.version + ', better-sqlite3 SQLite ' + v + ', ' + require(path.join(path.dirname(require('./bsq_helpers.js').BSQ), '..', 'package.json')).version);
  assert.match(v, /^3\.\d+\.\d+/);
});
test('A loads with the default entry point', (t) => { assert.deepEqual(infoOf(load(t)), INFO); });
test('A loads with each explicit entry point', (t) => {
  for (const entry of ['sqlite3_forkdb_init', 'sqlite3_extension_init']) {
    const c = new Database(':memory:');
    later(t, () => c.close());
    c.loadExtension(SO, entry);
    assert.deepEqual(infoOf(c), INFO, entry);
  }
});
test('A a missing library is an error', (t) => {
  const c = new Database(':memory:');
  later(t, () => c.close());
  assert.throws(() => c.loadExtension('/nonexistent/forkdb'), { name: 'SqliteError' });
});
test('A loads twice on one connection', (t) => {
  const c = load(t);
  c.loadExtension(SO);
  assert.deepEqual(infoOf(c), INFO);
});
test('A the library survives closing the connection that loaded it', (t) => {
  const loader = load(null);
  const store = path.join(tmpdir(t), 's.fk');
  loader.prepare('select forkdb_init(?)').get(store);
  const db = openBranch(t, loader, store, 'main');
  db.exec('create table t(a integer primary key)');
  db.exec('insert into t values (1),(2),(3)');
  loader.close();
  const other = load(t);
  later(t, () => { try { other.prepare('select forkdb_close(?)').get(store); } catch (e) { /* closed */ } });
  assert.equal(one(db, 'select count(*) from t'), 3);
  db.exec('insert into t values (100)');
  assert.equal(one(db, 'select count(*) from t'), 4);
  assert.deepEqual(infoOf(other), INFO);
});

test('hazard: with URI filenames on (SQLITE_USE_URI=1) a branch opens and works', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  const uri = one(c, "select forkdb_open_db(?, 'main')", store);
  assert.match(uri, /^file:\/forkdb\/b[0-9]+\?vfs=forkdb$/);
  const db = openBranch(t, c, store, 'main');
  db.exec('create table t(a)');
  db.exec('insert into t values (1)');
  assert.equal(one(db, 'select count(*) from t'), 1);
  assert.ok(fs.statSync(store).size > 0, 'the data went into the store');
});
test('hazard: without SQLITE_USE_URI the open FAILS and creates no file (it used to write to a plain file in the working directory)', (t) => {
  const c = load(t);
  const store = newStore(t, c, path.join(tmpdir(t), 's.fk'));
  const uri = one(c, "select forkdb_open_db(?, 'main')", store);
  const cwd = fs.mkdtempSync(path.join(require('node:os').tmpdir(), 'fk-bsq-cwd-'));
  later(t, () => fs.rmSync(cwd, { recursive: true, force: true }));
  // better-sqlite3 reads SQLITE_USE_URI when it is first loaded, so the proof needs a child without the variable.
  const r = runNode([
    'const d = (() => { try { return new Database(process.argv[3]); } catch (e) { console.log("refused: " + e.message); process.exit(0); } })();',
    'console.log("OPENED"); process.exit(3);',
  ].join('\n'), [store, uri], { env: { SQLITE_USE_URI: undefined }, cwd });
  assert.equal(r.status, 0, 'a plain open of the URI string must fail: ' + r.stdout + r.stderr);
  assert.match(r.stdout, /refused: unable to open database file/);
  assert.deepEqual(fs.readdirSync(cwd), [], 'the failed open must not create a file or directory');
});
