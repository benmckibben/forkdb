'use strict';
// Shared by the better-sqlite3 tests of the forkdb loadable extension (test/ext_bsq_*_test.js). The suite is run by scripts/test-ext-bsq.sh, which installs the pinned
// better-sqlite3 into build/bsq (never committed) and exports SQLITE_USE_URI=1 BEFORE node starts: better-sqlite3 is built with SQLITE_USE_URI=0 and reads that variable once,
// when the addon is first loaded. Without it forkdb_open_db's URI is read as a plain path and the open fails (see test 'hazard' in ext_bsq_test.js).
// Nothing here skips: a missing install or a missing variable fails the run.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { createRequire } = require('node:module');
const { spawnSync, spawn } = require('node:child_process');

const ROOT = path.dirname(__dirname);
const BSQ_DIR = path.join(ROOT, 'build', 'bsq');
let BSQ;
try {
  BSQ = createRequire(path.join(BSQ_DIR, 'package.json')).resolve('better-sqlite3');
} catch (e) {
  throw new Error('better-sqlite3 is not installed in ' + BSQ_DIR + ': run scripts/test-ext-bsq.sh (it installs the pinned version)');
}
if (process.env.SQLITE_USE_URI !== '1') {
  throw new Error('SQLITE_USE_URI=1 must be set before node starts (scripts/test-ext-bsq.sh does): better-sqlite3 reads it once, at load');
}
const Database = require(BSQ);
const SO = process.env.FK_EXT_SO || path.join(ROOT, 'build', 'ext', 'forkdb.' + (process.platform === 'win32' ? 'dll' : process.platform === 'darwin' ? 'dylib' : 'so'));
if (!fs.existsSync(SO)) throw new Error('no extension at ' + SO + ': run scripts/build-ext.sh');

// Cleanups run last-in first-out (like unittest's addCleanup): node:test runs t.after hooks in registration order, which would close the connection a store was
// opened through before forkdb_close could use it. All the helpers below register through this.
const stacks = new WeakMap();
function later(t, fn) {
  let stack = stacks.get(t);
  if (!stack) {
    stack = [];
    stacks.set(t, stack);
    t.after(() => {
      let first;
      while (stack.length) {
        try { stack.pop()(); } catch (e) { if (!first) first = e; }
      }
      if (first) throw first;
    });
  }
  stack.push(fn);
}

// Child-process scripts: argv[1] is the library, argv[2] a store path, argv[3...] test-specific (node -e puts the first user argument at argv[1]).
// They see Database, load(), one(), SO, P. better-sqlite3 is required by absolute path: the child's cwd is arbitrary.
const PRELUDE = [
  'const Database = require(' + JSON.stringify(BSQ) + ');',
  'const SO = process.argv[1], P = process.argv[2];',
  'function load() { const c = new Database(":memory:"); c.loadExtension(SO); return c; }',
  'function one(c, sql, ...a) { const r = c.prepare(sql).get(...a); return Object.values(r)[0]; }',
  '',
].join('\n');

// A fresh in-memory connection with the extension loaded (better-sqlite3 needs no allowExtension flag). Closed when the test ends.
function load(t) {
  const c = new Database(':memory:');
  c.loadExtension(SO);
  if (t) later(t, () => { try { c.close(); } catch (e) { /* already closed */ } });
  return c;
}

// First column of the first row.
function one(c, sql, ...args) {
  return Object.values(c.prepare(sql).get(...args))[0];
}

// A temp directory removed when the test ends.
function tmpdir(t) {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'fk-bsq-'));
  later(t, () => fs.rmSync(d, { recursive: true, force: true }));
  return d;
}

// forkdb_close that tolerates a store that is already closed.
function closeStore(c, store) {
  try { c.prepare('select forkdb_close(?)').get(store); } catch (e) { if (!/store not open/.test(e.message)) throw e; }
}

// Init a store (options optional), close it when the test ends.
function newStore(t, c, store, options) {
  if (options === undefined) c.prepare('select forkdb_init(?)').get(store);
  else c.prepare('select forkdb_init(?, ?, ?)').get(store, 'main', options);
  later(t, () => closeStore(c, store));
  return store;
}

// A read-write connection to a branch (a snapshot only opens with { readonly: true }).
function openBranch(t, c, store, ref, opts) {
  const uri = one(c, 'select forkdb_open_db(?, ?)', store, ref || 'main');
  const db = new Database(uri, opts || {});
  later(t, () => { try { db.close(); } catch (e) { /* already closed */ } });
  return db;
}

// Run JS in a child node (synchronously). Returns { status, stdout, stderr, signal }. opts: { env, cwd }; env REPLACES nothing: it is merged over process.env,
// and a key set to undefined is removed (runNode(code, [args], { env: { SQLITE_USE_URI: undefined } }) is a child without the variable).
function runNode(code, args, opts) {
  const o = opts || {};
  const env = { ...process.env };
  for (const [k, v] of Object.entries(o.env || {})) { if (v === undefined) delete env[k]; else env[k] = v; }
  return spawnSync(process.execPath, ['--no-warnings', '-e', PRELUDE + code, SO, ...(args || [])], { encoding: 'utf8', timeout: 120000, env, cwd: o.cwd });
}

function startNode(code, args, env) {
  return spawn(process.execPath, ['--no-warnings', '-e', PRELUDE + code, SO, ...(args || [])], {
    env: { ...process.env, ...(env || {}) }, stdio: ['pipe', 'pipe', 'pipe'],
  });
}

// Did a child that killed itself with process.kill(process.pid, 'SIGKILL') die that way? Windows has no signals: node ends the process with exit status 1 there.
function died(r) { return r.signal === 'SIGKILL' || (process.platform === 'win32' && r.status === 1 && !r.signal); }

module.exports = { died, later, Database, BSQ, SO, ROOT, PRELUDE, load, one, tmpdir, closeStore, newStore, openBranch, runNode, startNode };
