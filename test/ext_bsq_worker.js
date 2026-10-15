'use strict';
// A writer for ext_bsq_threads_test.js: runs in a worker thread, so its better-sqlite3 connection really runs in parallel with the others.
// It loads the extension on its own connection, opens its branch through the process-global registry and commits workerData.iters rows.
const { workerData, parentPort } = require('node:worker_threads');
const { bsq, so, store, ref, iters, base } = workerData;
const Database = require(bsq);

const host = new Database(':memory:');
host.loadExtension(so);
const uri = Object.values(host.prepare('select forkdb_open_db(?, ?)').get(store, ref))[0];
const db = new Database(uri, { timeout: 10000 });
const sleep = (ms) => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);

let ok = 0;
const errors = [];
for (let k = 0; k < iters && errors.length === 0; k++) {
  let done = false;
  for (let attempt = 0; attempt < 600 && !done; attempt++) {
    try {
      db.exec('begin immediate');
      db.prepare("insert into t values (?, 'w')").run(base + k);
      db.exec('commit');
      ok++;
      done = true;
    } catch (e) {
      try { db.exec('rollback'); } catch (e2) { /* no transaction */ }
      if (/locked|busy/i.test(e.message)) sleep(Math.random() * 2);
      else { errors.push(String(e.message)); break; }
    }
  }
  if (!done && errors.length === 0) errors.push('gave up on commit ' + k);
}
const check = db.pragma('integrity_check');
if (check.length !== 1 || check[0].integrity_check !== 'ok') errors.push('integrity_check: ' + JSON.stringify(check));
db.close();
host.close();
parentPort.postMessage({ ok, errors });
