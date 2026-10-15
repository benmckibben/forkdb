'use strict';
// Real threads with better-sqlite3: node worker_threads, one connection per worker, all running in parallel inside the forkdb VFS (better-sqlite3 is synchronous, so
// worker threads are the only way to get concurrency in a Node process). The same cases as group F of ext_node_test.js (node:sqlite).
const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const { Worker } = require('node:worker_threads');
const { BSQ, SO, load, one, tmpdir, newStore, openBranch, closeStore } = require('./bsq_helpers.js');

const seed = (db, n) => {
  db.exec('create table t(a integer primary key, b text)');
  db.exec('begin');
  const ins = db.prepare('insert into t values (?, ?)');
  for (let i = 0; i < n; i++) ins.run(i, 'x');
  db.exec('commit');
};
const count = (db) => one(db, 'select count(*) from t');
const integrity = (db) => db.pragma('integrity_check').map((r) => r.integrity_check);

const worker = (store, ref, i, iters) => new Promise((resolve, reject) => {
  const w = new Worker(path.join(__dirname, 'ext_bsq_worker.js'), { workerData: { bsq: BSQ, so: SO, store, ref, iters, base: 100000 * (i + 1) } });
  w.once('message', resolve);
  w.once('error', reject);
  w.once('exit', (code) => { if (code !== 0) reject(new Error('worker exited with ' + code)); });
});
const threadStore = (t) => {
  const c = load(t);
  const store = path.join(tmpdir(t), 's.fk');
  newStore(t, c, store);
  seed(openBranch(t, c, store, 'main'), 200);
  for (const n of ['a', 'b', 'c']) one(c, "select forkdb_fork(?, 'main', ?)", store, n);
  return { c, store };
};
const settle = (t, c, store, refs, results, iters) => {
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

test('bsq F writers on separate branches, one worker thread each', async (t) => {
  const { c, store } = threadStore(t);
  const refs = ['main', 'a', 'b', 'c'];
  const results = await Promise.all(refs.map((r, i) => worker(store, r, i, 120)));
  settle(t, c, store, refs, results, 120);
});
test('bsq F writers on the same branch, one worker thread each', async (t) => {
  const { c, store } = threadStore(t);
  const refs = ['a', 'a', 'a'];
  const results = await Promise.all(refs.map((r, i) => worker(store, r, i, 100)));
  settle(t, c, store, refs, results, 100);
});
test('bsq F branch management on this thread while workers write', async (t) => {
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
  settle(t, c, store, refs, results, 100);
});
