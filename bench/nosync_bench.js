'use strict';
// What nosync saves, measured: the time per operation and the number of durability calls (forkdb_sync_count) per operation, for a durable store and for one opened with 'nosync',
// on whatever disk FK_BENCH_DIR (default: the temp directory) is on. Run by scripts/bench-nosync.sh, and in CI on Linux, macOS and Windows runners.
//
// It asserts only what is deterministic: a nosync store makes no durability calls, and a durable store makes at least one per operation. It asserts nothing about time, which varies
// from run to run and from disk to disk: the table is for reading. Each cell is the median of 5 runs (each on a fresh store), after a warm-up.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { DatabaseSync } = require('node:sqlite');

const SO = process.env.FK_EXT_SO;
if (!SO || !fs.existsSync(SO)) throw new Error('FK_EXT_SO must name the built extension (scripts/bench-nosync.sh sets it)');
const base = fs.mkdtempSync(path.join(process.env.FK_BENCH_DIR || os.tmpdir(), 'fk-bench-'));
process.on('exit', () => { try { fs.rmSync(base, { recursive: true, force: true }); } catch (e) { /* best effort */ } });

const host = new DatabaseSync(':memory:', { allowExtension: true });
host.loadExtension(SO);
const one = (sql, ...a) => Object.values(host.prepare(sql).get(...a))[0];
const syncs = () => Number(one('select forkdb_sync_count()'));
let seq = 0;

function fresh(nosync) {
  const store = path.join(base, 's' + (seq++) + '.fk');
  if (nosync) host.prepare("select forkdb_init(?, 'main', 'nosync')").get(store);
  else host.prepare('select forkdb_init(?)').get(store);
  const db = new DatabaseSync(one("select forkdb_open_db(?, 'main')", store));
  db.exec('create table t(a integer primary key, b text)');
  return { store, db, next: 0 };
}
const finish = (s) => { s.db.close(); host.prepare('select forkdb_close(?)').get(s.store); fs.rmSync(s.store, { force: true }); };

const OPS = {
  'commit (insert one row)': { k: 300, run(s, k) { const ins = s.db.prepare('insert into t values (?, ?)'); for (let i = 0; i < k; i++) ins.run(s.next++, 'x'.repeat(100)); } },
  'fork + discard': { k: 150, run(s, k) { for (let i = 0; i < k; i++) { host.prepare("select forkdb_fork(?, 'main', 'f')").get(s.store); host.prepare("select forkdb_discard(?, 'f')").get(s.store); } } },
  'branch per test (fork, write, commit, close, discard)': { k: 100, run(s, k) {
    for (let i = 0; i < k; i++) {
      host.prepare("select forkdb_fork(?, 'main', 'f')").get(s.store);
      const b = new DatabaseSync(one("select forkdb_open_db(?, 'f')", s.store));
      b.prepare('insert into t values (?, ?)').run(i, 'y');
      b.close();
      host.prepare("select forkdb_discard(?, 'f')").get(s.store);
    }
  } },
};

function measure(op, nosync) {
  const s = fresh(nosync);
  op.run(s, 10); // warm-up: the first calls pay for page cache and allocation
  const n0 = syncs();
  const t0 = process.hrtime.bigint();
  op.run(s, op.k);
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  const n = syncs() - n0;
  finish(s);
  return { msPerOp: ms / op.k, syncsPerOp: n / op.k, syncs: n };
}
const median = (xs) => xs.slice().sort((a, b) => a - b)[Math.floor(xs.length / 2)];

const rows = [];
let bad = 0;
for (const [name, op] of Object.entries(OPS)) {
  const res = {};
  for (const nosync of [false, true]) {
    const runs = [];
    for (let i = 0; i < 5; i++) runs.push(measure(op, nosync));
    res[nosync ? 'nosync' : 'durable'] = { ms: median(runs.map((r) => r.msPerOp)), syncs: median(runs.map((r) => r.syncsPerOp)), total: runs.map((r) => r.syncs) };
  }
  if (res.nosync.total.some((n) => n !== 0)) { console.error('FAIL: a nosync store made durability calls in ' + name + ': ' + res.nosync.total); bad++; }
  if (!(res.durable.syncs >= 1)) { console.error('FAIL: a durable store made fewer than one durability call per operation in ' + name + ': ' + res.durable.syncs); bad++; }
  rows.push({ name, ...res });
}

const f = (x, d) => x.toFixed(d);
const lines = [
  '| operation | durable: syncs/op | durable: ms/op | nosync: syncs/op | nosync: ms/op | speed-up |',
  '|---|---|---|---|---|---|',
  ...rows.map((r) => '| ' + [r.name, f(r.durable.syncs, 2), f(r.durable.ms, 3), f(r.nosync.syncs, 2), f(r.nosync.ms, 4), f(r.durable.ms / r.nosync.ms, 0) + 'x'].join(' | ') + ' |'),
];
console.log(lines.join('\n'));
console.log('\nnode ' + process.version + ', ' + os.platform() + ' ' + os.release() + ', ' + os.cpus().length + ' x ' + (os.cpus()[0] || {}).model + '; median of 5 runs per cell, after a warm-up');
process.exit(bad ? 1 : 0);
