# sqlite-forkdb

The [forkdb](https://github.com/benmckibben/forkdb) SQLite extension, prebuilt, with `load()` for Node. **Pre-release (0.1.0): everything may change** (see docs/STABILITY.md in the repository). No dependencies.

    npm install sqlite-forkdb

```js
const { DatabaseSync } = require('node:sqlite');
const forkdb = require('sqlite-forkdb');
const host = forkdb.load(new DatabaseSync(':memory:', { allowExtension: true }));
host.prepare('select forkdb_init(?)').get('/tmp/s.fk');
const uri = Object.values(host.prepare("select forkdb_open_db(?, 'main')").get('/tmp/s.fk'))[0];
const main = new DatabaseSync(uri);
```

With better-sqlite3: `forkdb.load(new Database(':memory:'))`, **and set `SQLITE_USE_URI=1` in the environment before `require('better-sqlite3')`**: it is built without URI filenames, and opening a branch fails without it.

Prebuilt for linux-x64 (glibc 2.28 or later), darwin-arm64 and win32-x64; not musl. Elsewhere build the extension from the repository and set `FORKDB_EXTENSION_PATH`. Tested on Node 24. The SQLite behind your driver must be 3.24 or later.
