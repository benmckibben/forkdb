# Changelog

## Unreleased

Nothing has been released: there is no tag and no release (see [docs/STABILITY.md](docs/STABILITY.md)). Entries are the merged pull requests, 9 to 15 October 2026, grouped by area and worded after their titles; the number is the pull request. #36 to #38 (language SDKs that wrapped the CLI) were closed without merging.

### The store (the append-only log)

- Durable layer store and power-loss test harness (#3). Compaction and garbage collection (#4). Faster checksums, a sparse layer index, reusable branch ids (#6). Dead layers freed, single-child layers folded when a branch is discarded (#8). `fk_txn_write_owned` hands a page to the store without a copy (#11).
- Format version 4: branch metadata, reset and state ids (#18).
- The log can be preallocated, opt-in (#15); zero-filling in the background between commits (#25), wired into the branch layer's writes (#27), without an fsync (#31).
- Open-time index checkpoint and auto-compaction (#23).
- Run without fsync for throwaway stores, `forkdb_init(path, root, 'nosync')` (#44). A sync counter that works on every platform, `fk_file_sync_count()` and `forkdb_sync_count()`; the `nosync` tests run on macOS and Windows; a power-loss pass for `nosync` stores under SQLite; a benchmark of what `nosync` saves, run on all three platforms in CI (#54).

### The VFS, the branch layer, the SQL functions and the CLI

- A SQLite VFS on the durable store (#9); batch-atomic writes dropped from the design (#10); the session build, the one-branch-write contract and a host survey (#17).
- The branch layer `fkx`: design (#19), core: branches, fork, snapshot, reset, discard, fast-forward promote and undo (#20), session driver: diff, promote and undo by replay (#21). Promote replays DDL for new tables, indexes and triggers (#26) and virtual table DDL (#33).
- The `forkdb_*` SQL functions and the `forkdb` CLI (#22).

### Platforms and hosts

- macOS: `fdatasync` fallback and CI (#34). Windows: an I/O backend (#35).
- A portable loadable extension that loads into any host SQLite (3.24 and later), two host crashes fixed, a Python test suite (#39).
- Tested from Node's built-in `node:sqlite` (#46) and from better-sqlite3 (#48); built and tested on macOS and Windows, and in SQLite 3.24.0 (#52). A pytest plugin that gives every test a fresh database from a branch (#43).

### Fixes (most found by tests added after the code they test)

- The CLI segfaulted on a store it could not open (#40).
- Two processes writing one store file could lose acknowledged commits; a store now has one opener at a time (#41).
- The store was not safe to use from several threads; the VFS and `fkx` now take one mutex, with a ThreadSanitizer test (#42).
- Compaction failed on every Windows machine (#45); the extension's library was not pinned on Windows and crashed the host when the connection that loaded it closed (#52).
- Branch ids were 64-bit numbers, and `node:sqlite` throws on an integer above 2^53 after the fork has already happened: ids are 53 bits (#46).
- A driver that cannot read the URI `forkdb_open_db` returns silently wrote into a plain file of that name: the string is now one such a driver cannot open (#47).
- `forkdb_init`, or any function that opens a store, replaced a file of up to 4096 bytes that was not a store (a 100-byte notes file, a one-page SQLite database) with an empty store; such a file is now refused and left alone (#51). A store whose very first creation was interrupted by a power cut may be refused for the same reason: delete it and create it again.

### Tests, CI and measurement

- CI workflow (#28); OOM fault injection (#29, #32); corrupted-record recovery tests (#30, #32); a recovery fuzzer with a mutant check (#50).
- Benchmarks and a real-disk measurement (#5, #12, #13, #14). The OpenTofu host definition they added under `infra/bench` has been removed from the tree (it is in history).

### Documentation and licence

- Scaffold, design decision and feasibility spike (#1); MIT and Apache-2.0 licences (#2); README claims limited to what was measured (#7); release documentation (#24); what the durability claim rests on, a simulated disk and not a real power cut (#49).

