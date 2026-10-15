'use strict';
// Shared by the Node tests of the forkdb loadable extension (test/ext_node_*.js). Node's built-in node:sqlite is the host: it brings its own SQLite
// (a different build from the one the C tests and Python use), and loadExtension needs { allowExtension: true }.
// Nothing here skips: a Node without node:sqlite loadExtension fails the run, because a skipped suite proves nothing.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync, spawn } = require('node:child_process');
const sqlite = require('node:sqlite');

if (!sqlite.DatabaseSync || typeof sqlite.DatabaseSync.prototype.loadExtension !== 'function') {
  throw new Error('this Node (' + process.version + ') has no node:sqlite DatabaseSync.loadExtension: the suite cannot run here');
}
const { DatabaseSync } = sqlite;

const ROOT = path.dirname(__dirname);
const SO = process.env.FK_EXT_SO || path.join(ROOT, 'build', 'ext', 'forkdb.so');
if (!fs.existsSync(SO)) throw new Error('no extension at ' + SO + ': run scripts/build-ext.sh');

// Shared by the child-process scripts: argv[1] is the library, argv[2] a store path, argv[3...] test-specific (node -e puts the first user argument at argv[1]).
const PRELUDE = [
  "const { DatabaseSync } = require('node:sqlite');",
  'const SO = process.argv[1], P = process.argv[2];',
  'function load() { const c = new DatabaseSync(":memory:", { allowExtension: true }); c.loadExtension(SO); return c; }',
  'function one(c, sql, ...a) { return Object.values(c.prepare(sql).get(...a))[0]; }',
  '',
].join('\n');

// Cleanups run last-in first-out (like unittest's addCleanup): node:test runs t.after hooks in the order they were registered, which would close
// the connection a store was opened through before forkdb_close could use it. All the helpers below register through this.
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

// A fresh connection with the extension loaded. Closed when the test ends.
function load(t) {
  const c = new DatabaseSync(':memory:', { allowExtension: true });
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
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'fk-node-'));
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

// A read-write connection to a branch (a snapshot only opens read-only).
function openBranch(t, c, store, ref, opts) {
  const uri = one(c, 'select forkdb_open_db(?, ?)', store, ref || 'main');
  const db = new DatabaseSync(uri, opts || {});
  later(t, () => { try { db.close(); } catch (e) { /* already closed */ } });
  return db;
}

// Run JS in a child node (synchronously). The script sees load(), one(), SO, P. Returns { status, stdout, stderr, signal }.
function runNode(code, ...args) {
  return spawnSync(process.execPath, ['--no-warnings', '-e', PRELUDE + code, SO, ...args], { encoding: 'utf8', timeout: 120000 });
}

// The same, asynchronously, with extra environment (for LD_PRELOAD) and stdin piped; returns the ChildProcess.
function startNode(code, args, env) {
  return spawn(process.execPath, ['--no-warnings', '-e', PRELUDE + code, SO, ...args], {
    env: { ...process.env, ...(env || {}) }, stdio: ['pipe', 'pipe', 'pipe'],
  });
}

module.exports = { later, DatabaseSync, SO, ROOT, PRELUDE, load, one, tmpdir, closeStore, newStore, openBranch, runNode, startNode };
