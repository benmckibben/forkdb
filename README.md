# forkdb

A branchable, snapshot-based SQLite page store with a library, a loadable extension, and a CLI.

On top of an append-only log with power-loss recovery (tested against a simulated disk: see [Durability](#durability-what-is-proven-and-against-what)), forkdb layers branching (fork, snapshot, diff, promote, undo), garbage collection, and an open-time checkpoint so frequent opens stay fast. Applications connect through a [SQLite VFS](docs/VFS-SPIKE.md) that enforces one-writer-per-branch.

## What you need

forkdb is a C library that requires:

- A SQLite built with the **session API** (`SQLITE_ENABLE_SESSION`, `SQLITE_ENABLE_PREUPDATE_HOOK`) for diff/promote/undo. The CLI and the library are built with it. The portable loadable extension (`scripts/build-ext.sh`) is not: the session API is not in the table SQLite gives an extension, so its `diff`, `promote` and `undo` report that this host has none (`forkdb_info.session_api` is 0), and the VFS and branch operations work.
- A file system that supports **rename over an open file** for compaction (otherwise the store works without it).
- **Single-writer.** One writer transaction at a time, across all branches, enforced by the VFS. Reads from separate connections on different branches are fine.

## Quick start

```sh
# Build the CLI (offline, for inspecting stores no app has open)
./scripts/build-step4.sh cli        # produces build/forkdb

# Build the extension for the sqlite3 shell (needs the symbols of a host that exports SQLite)
./scripts/build-step4.sh ext        # produces build/forkdb.so

# Build the portable extension: loads into any SQLite 3.24+ (Python's sqlite3, Node's built-in node:sqlite), no session driver
./scripts/build-ext.sh              # produces build/ext/forkdb.so
./scripts/test-ext.sh               # builds it and runs test/ext_py_test.py through Python's sqlite3
./scripts/test-ext-node.sh          # builds it and runs test/ext_node*_test.js through Node 24's built-in node:sqlite
./scripts/test-ext-bsq.sh           # installs better-sqlite3 13.0.3 (npm, into build/) and runs test/ext_bsq*_test.js through it

# Build both plus the test suite
./scripts/build-step4.sh test
```

The CLI binary is self-contained: it links against its own SQLite. Use it on a store no application has open: the store file is locked while it is open, so the CLI (or a second application) is refused with an error if another process holds it.

## CLI

```
forkdb init <path> [branch]         create a store, default branch "main"
forkdb fork <path> <name> [--from]  branch off another branch
forkdb snapshot <path> <name>       create a named snapshot
forkdb list <path>                  list branches
forkdb info <path> [branch]         show branch metadata and state
forkdb diff <path> <branch>         show changes relative to the base
forkdb promote <path> <branch>      promote a branch into its base (fast-forward or replay)
forkdb undo <path> <branch>         undo the last promote
forkdb discard <path> <branch>      remove a branch
forkdb reset <path> <branch> <to>   reset a branch's content to another
forkdb gc <path>                    garbage-collect unreachable undo points
forkdb compact <path>               compact the log to a clean snapshot
forkdb sql <path> <branch> [sql]    run SQL on a branch (--write for writes)
forkdb shell <path> <branch>        open an interactive SQLite shell
```

All commands accept `--json` for machine-readable output.

## Extension

```sql
.load forkdb

-- Check if the host has the session API
SELECT * FROM forkdb_info;

-- Create a store and branch off (fork takes the store, the branch to fork from, and the new name)
SELECT forkdb_init('/tmp/mine.fk', 'main');
SELECT forkdb_fork('/tmp/mine.fk', 'main', 'feat-x');

-- Get the URI of a branch and open it with a second connection
SELECT forkdb_open_db('/tmp/mine.fk', 'feat-x');  -- file:/forkdb/b1?vfs=forkdb
-- ... sqlite3.connect(uri, uri=True) in Python, sqlite3_open_v2 in C ...
SELECT forkdb_close('/tmp/mine.fk');
```

The URI names the branch by its store id (`b<N>`), which is only stable until the next fork, discard or reset on that store: open it right away.

The SQL functions are `forkdb_init(path[, root[, options]])` (the one option, `'nosync'`, skips every fsync for a throwaway store: about 100 times faster fork and discard on a disk, safe against the death of the process, not against a power cut), `forkdb_fork(path, from, name)`, `forkdb_snapshot(path, from, name)`, `forkdb_discard(path, name)`, `forkdb_reset(path, name[, to])`, `forkdb_compact(path)`, `forkdb_vfs_name(path)`, `forkdb_open_db(path[, ref])`, `forkdb_close(path)`, and `forkdb_diff`, `forkdb_promote` and `forkdb_undo` where the session API is present. The table `forkdb_info` reports the session API, the VFS name and the version. They are direct-only: a view or trigger cannot call them. A store that cannot be opened is an SQL error. The extension stays loaded for the life of the process (it registers a VFS).

The other [fkx](docs/FKX-DESIGN.md) operations (list, info, gc) are in the CLI and the C library, not in SQL.

**A worked example: a fresh database for every pytest test.** [examples/pytest-branch-per-test](examples/pytest-branch-per-test/README.md) is a pytest plugin that gives each test its own branch of a seeded database, a 116-test suite and isolation canaries for it, and measurements of what it costs (cheap on tmpfs and for large seeds; for a small seed on a disk a file copy is as good).

## How it works

The store is an **append-only log** (see [DURABLE-STORE.md](docs/DURABLE-STORE.md)). Recovery truncates a torn tail, replays the committed prefix, and rebuilds an in-memory index. On open, if the log exceeds 16 MiB, the store compacts it to a clean snapshot — the next open loads instantly.

The **branch layer** ([FKX-DESIGN.md](docs/FKX-DESIGN.md)) keeps branches in a tree of layers. A fork copies nothing — it creates an empty child layer that reads through to the parent. Discard folds the smaller layer into the survivor, keeping reads fast. Promote replays or fast-forwards a branch's changes into its base, recording an undo point. GC collects unreachable undo points.

The **session driver** ([SESSION-DESIGN.md](docs/SESSION-DESIGN.md)) uses SQLite's session extension for diff, conflict detection, replay, and undo. Schema changes and tables without a PRIMARY KEY are detected and reported; the policy controls whether they block the promote.

## Durability: what is proven, and against what

A commit that returns is meant to survive a power cut, and recovery is meant to yield exactly the state after the last acknowledged operation (or that plus the one operation that was in flight). **That is tested against a simulated disk, not against a real power cut. No real disk was power-cycled, for either mode.**

- **The simulated disk** ([test/simdisk.c](test/simdisk.c), driven by [test/crash_test.c](test/crash_test.c)) is deliberately pessimistic: writes are pending until an fsync, and at a power cut the surviving image is the durable data plus *any subset* of the pending writes, in *any order*, each possibly torn. Seeded workloads (commits, forks, discards, compactions) are cut before every I/O event, including the I/O events of recovery itself and of a compaction, and after a failed fsync. Targeted mutants (for example, commits acknowledged without an fsync) are caught. [DURABLE-STORE.md](docs/DURABLE-STORE.md) has the details and the mutants that were *not* caught.
- **What the model assumes:** 512-byte sector atomicity, that fsync really makes data durable, and that data does not rot while the machine runs. A drive that lies about fsync, or a file system that loses a file's size update, is outside it. File systems that cannot lock or do not honour fsync (some network mounts) get no protection.
- **`nosync` stores** (`forkdb_init(path, root, 'nosync')`, for throwaway stores) survive the death of the process, which is tested with real kills. After a power cut they either refuse to open or open to a state the workload had acknowledged, usually an older one, never one it did not have. That claim also rests on the simulator.
- **Windows:** the store calls `FlushFileBuffers`, and compaction needs NTFS on Windows 10 1709 or later. The directory sync after a compaction is best effort. Windows runs in CI as a smoke test, with a flush counter, and nothing was tested against a power cut there.
- **Tested on:** Linux in CI (every suite, and the thread tests under ThreadSanitizer), and the store tests on macOS and Windows in CI; SQLite 3.50.0 is pinned for the C tests. Not tested: other operating systems, the extension suites on macOS and Windows, or a power cut on any of them.

If you need the durable guarantee, run your own power-cut test on your hardware and file system before relying on it.

## Known limitations

See [KNOWN-LIMITS.md](docs/KNOWN-LIMITS.md). The short version:

- Tables without a PRIMARY KEY are not compared (diff reports them, promote refuses them with FKX_TABLE_NO_PK)
- DDL changes are detected but not replayed
- Virtual tables and FTS shadow tables are not compared
- Large changesets are held in memory (a streaming interface exists but is not exposed to callers yet)
- Single-writer (one writer at a time, reads from separate connections are fine)
- Durability is tested against a simulated disk, not a real power cut (see Durability above)
- better-sqlite3 needs the environment variable `SQLITE_USE_URI=1` before it is required; without it opening a branch fails (see KNOWN-LIMITS.md)

## Documentation

- [DURABLE-STORE.md](docs/DURABLE-STORE.md) — the append-only log, recovery, snapshots, branch ids, state ids, compaction
- [FKX-DESIGN.md](docs/FKX-DESIGN.md) — the branch layer: forks, layers, diff, promote, undo, garbage collection
- [SESSION-DESIGN.md](docs/SESSION-DESIGN.md) — session driver design and build plan
- [VFS-SPIKE.md](docs/VFS-SPIKE.md) — VFS contract, one-branch-write enforcement, NVMe cost analysis
- [SPIKE-FINDINGS.md](docs/SPIKE-FINDINGS.md) — early investigation notes

## License

Licensed under either of [Apache License, Version 2.0](LICENSE-APACHE) or
[MIT license](LICENSE-MIT) at your option.
