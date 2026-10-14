# forkdb Node.js Bindings

Node.js bindings for [forkdb](https://github.com/benmckibben/forkdb) - a branchable, snapshot-based SQLite page store.

## Installation

```bash
npm install forkdb
```

## Usage

```javascript
const forkdb = require('forkdb');

// Create and open a database
const db = forkdb.open('/path/to/database.fk');

// Or create manually
const db = new forkdb.ForkDB('/path/to/database.fk');
db.open();

// Create a branch
db.fork('main', 'feature-branch');

// List all branches
const branches = db.list();
console.log(branches);

// Get branch information
const info = db.info('feature-branch');
console.log(info);

// Promote changes back to main
const result = db.promote('feature-branch');
console.log(result);

// Close the database
db.close();
```

## API

### ForkDB

#### Constructor

```javascript
const db = new forkdb.ForkDB(path);
```

Creates a new ForkDB instance for the given database path.

#### Methods

##### `open([path])`

Opens the database. If path is provided, it overrides the path given in the constructor.

##### `close()`

Closes the database and frees resources.

##### `fork(from, name)`

Creates a new branch named `name` from the branch `from`.

##### `snapshot(from, name)`

Creates a read-only snapshot named `name` from the branch `from`.

##### `discard(name)`

Removes the branch `name` and all associated data.

##### `reset(name, to)`

Resets the branch `name` to match the content of branch `to`.

##### `info(ref)`

Returns information about the branch referenced by `ref`.

##### `list([includeInternal])`

Lists all branches. If `includeInternal` is true, includes internal branches.

##### `promote(name[, options])`

Promotes the branch `name` to its target.

Options:
- `into`: Target branch (defaults to the fork's origin)
- `dryRun`: Run without making changes

##### `undo(name)`

Undoes the last promotion into branch `name`.

##### `gc()`

Performs garbage collection on the repository.

##### `compact([minBytes])`

Compacts the repository if it exceeds `minBytes`.

## Building from Source

To build from source, you need:

- Node.js v14 or later
- node-gyp
- A C++ compiler

```bash
npm install
npm run build
```

## License

MIT or Apache 2.0 at your option.