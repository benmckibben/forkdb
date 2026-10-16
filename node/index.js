'use strict';
// sqlite-forkdb: the forkdb SQLite extension, prebuilt, and load() for node:sqlite and better-sqlite3.
// Unstable first release (0.1.0): everything may change.
const fs = require('node:fs');
const path = require('node:path');

const SUFFIX = { win32: 'dll', darwin: 'dylib' }[process.platform] || 'so';

function isMusl() {
  if (process.platform !== 'linux') return false;
  try { return !process.report.getReport().header.glibcVersionRuntime; } catch (e) { return false; }
}

function extensionPath() {
  const override = process.env.FORKDB_EXTENSION_PATH;
  if (override) {
    if (!fs.existsSync(override)) throw new Error('FORKDB_EXTENSION_PATH=' + override + ' does not exist');
    return override;
  }
  if (isMusl()) throw new Error('sqlite-forkdb has no prebuilt library for musl libc (Alpine); build the extension from the repository and set FORKDB_EXTENSION_PATH');
  const dir = process.platform + '-' + process.arch;
  const file = path.join(__dirname, 'lib', dir, 'forkdb.' + SUFFIX);
  if (!fs.existsSync(file)) {
    throw new Error('sqlite-forkdb has no prebuilt library for ' + dir + ' (expected ' + file + '). Prebuilt: linux-x64, darwin-arm64, win32-x64. ' +
      'Elsewhere build the extension from the repository and set FORKDB_EXTENSION_PATH.');
  }
  return file;
}

// Loads the extension into a node:sqlite DatabaseSync (opened with { allowExtension: true }) or a better-sqlite3 Database. Returns db.
function load(db) {
  const file = extensionPath();
  if (!db || typeof db.loadExtension !== 'function') throw new TypeError('load(db) needs a node:sqlite DatabaseSync or a better-sqlite3 Database');
  try {
    db.loadExtension(file);
  } catch (e) {
    if (e && e.code === 'ERR_INVALID_STATE' && /extension loading is not allowed/i.test(String(e.message))) {
      throw new Error('node:sqlite refused to load the extension: open the database with new DatabaseSync(path, { allowExtension: true })');
    }
    throw e;
  }
  return db;
}

// forkdb_open_db returns a URI. node:sqlite understands it; better-sqlite3 is built without URI support, and needs SQLITE_USE_URI=1 in the environment before it is first required
// (otherwise opening a branch fails with "unable to open database file").
module.exports = { load, extensionPath, version: require('./package.json').version };
