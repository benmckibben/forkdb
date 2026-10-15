'use strict';
// Portable smoke test of the sqlite-forkdb package on any platform: loads the extension into node:sqlite, makes a branch, writes, checks isolation. No LD_PRELOAD, no signals.
// By default it uses node/ in this repository (FORKDB_EXTENSION_PATH names the built library); with FORKDB_SMOKE_INSTALLED=1 it uses the installed package and its bundled library.
const path = require('node:path');
const fs = require('node:fs');
const os = require('node:os');
const { DatabaseSync } = require('node:sqlite');
const forkdb = process.env.FORKDB_SMOKE_INSTALLED ? require('sqlite-forkdb') : require(path.join(__dirname, '..', 'node'));

const failures = [];
function check(cond, what) { console.log((cond ? 'ok   ' : 'FAIL ') + what); if (!cond) failures.push(what); }
const one = (db, sql, ...a) => Object.values(db.prepare(sql).get(...a))[0];

console.log('node', process.version, '| platform', process.platform + '-' + process.arch, '| sqlite-forkdb', forkdb.version);
console.log('library:', forkdb.extensionPath());
let refused = false;
try { forkdb.load(new DatabaseSync(':memory:')); } catch (e) { refused = /allowExtension/.test(e.message); }
check(refused, 'load() on a database opened without allowExtension says what to do');
const host = forkdb.load(new DatabaseSync(':memory:', { allowExtension: true }));
const info = Object.fromEntries(host.prepare('select key, value from forkdb_info').all().map((r) => [r.key, r.value]));
check(info.vfs === 'forkdb', 'forkdb_info reports the vfs');
check(info.version === forkdb.version, "the library's version (" + info.version + ") is the package's (" + forkdb.version + ')');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fk-smoke-'));
const store = path.join(dir, 's.fk');
host.prepare('select forkdb_init(?)').get(store);
const open = (ref, opts) => new DatabaseSync(one(host, 'select forkdb_open_db(?, ?)', store, ref), opts || {});
const main = open('main');
main.exec('create table t(a integer primary key, b text)');
main.exec('begin');
for (let i = 0; i < 50; i++) main.prepare('insert into t values (?, ?)').run(i, 'x');
main.exec('commit');
one(host, "select forkdb_fork(?, 'main', 'a')", store);
const a = open('a');
check(one(a, 'select count(*) from t') === 50, "a fork starts with its parent's rows");
a.exec("update t set b = 'A' where a < 10");
a.exec("insert into t values (1000, 'new')");
check(one(a, "select count(*) from t where b = 'A'") === 10, 'the fork sees its own edits');
check(one(main, "select count(*) from t where b = 'A'") === 0, "main does not see the fork's edits");
check(one(main, 'select count(*) from t') === 50, 'main still has its 50 rows');
check(one(a, 'pragma integrity_check') === 'ok', 'integrity_check on the fork');
one(host, "select forkdb_snapshot(?, 'main', 's')", store);
const s = open('s', { readOnly: true });
check(one(s, 'select count(*) from t') === 50, 'a snapshot reads');
let refusedWrite = false;
try { s.exec("insert into t values (2000, 'z')"); } catch (e) { refusedWrite = /readonly/i.test(e.message); }
check(refusedWrite, 'a snapshot refuses writes');
for (const c of [main, a, s]) c.close();
host.prepare('select forkdb_close(?)').get(store);
host.close();
if (failures.length) { console.log('smoke_node FAILED: ' + failures.length + ' check(s)'); process.exit(1); }
console.log('smoke_node OK');
