# Known limitations (forkdb v0.1.0)

## Tables without a PRIMARY KEY

Tables without a PRIMARY KEY are not compared. The diff reports them as `FKX_TABLE_NO_PK` and `rows_differ` says whether their rows differ at all (checked with `EXCEPT`). A promote refuses them unless `policy.no_pk` is set to `FKX_REPLAY` in the caller. Fast-forward is still allowed.

This is a v0.1.0 limitation. The workaround: add a `PRIMARY KEY` (including `rowid` aliases) to every table you want to diff.

## DDL changes

When a fork creates new tables, indexes, or triggers, promote replays their DDL statements onto the target before applying the changeset. This handles the common case of a test branch or feature branch that adds new schema objects.

Alterations to existing objects (`ALTER TABLE`, replaced triggers) and dropped objects are detected and refused with `FKX_EREFUSED`, with the reason named. A diff reports altered tables as `FKX_TABLE_SCHEMA` when their definitions differ.

## Virtual tables and FTS

Virtual tables and FTS shadow tables are not compared. Diff reports them as `FKX_TABLE_VIRTUAL`.

## Large changesets

Changesets are held in memory during diff, promote, and undo. SQLite's session API provides a streaming interface (`sqlite3changeset_start` iterators), but the library buffers the full changeset blob. A diff on a very large change (millions of rows) will use proportional memory.

## Concurrency

- **One writer per branch.** SQLite's lock allows one write transaction at a time on a branch. Connections on different branches can each have a write transaction open at the same time and commit independently: measured with two connections, `main` and a fork, interleaving `begin immediate`, a write and a commit on each (both committed, `integrity_check` ok on both); the thread tests also run a writer per branch. (This section used to say "one writer transaction at a time, across all branches", which that measurement contradicts.) The store is thread-safe only through the VFS and fkx, which take one mutex around everything: a connection per thread is fine, but threads on different branches take turns inside the VFS (a commit, a read of a page) rather than run in parallel. Used directly (fkstore.h) the store is not thread-safe.
- **One writer per branch.** The VFS enforces this: a transaction on one branch and another on a different branch in the same connection opens a super-journal, which the VFS refuses (with a reason logged). Separate connections each on their own branch, reading or writing, are fine.
- **The thread tests do not show every lock to be needed or sufficient.** Removing the mutex from the VFS file's write wrapper (`fkf_write_m`, `vfs/fkvfs.c`) leaves `scripts/test-threads.sh` passing with no ThreadSanitizer warning (measured, quick and full runs), so that path is untested rather than known safe. The session driver (diff, promote, undo) is not tested under threads, and `fkx_store_rc` and `fkx_sqlite_rc` read their fields without the lock.
- **One process per store file.** The file is locked while open; a second process (the CLI while an application has the store open, two workers on one path) gets an error, not a damaged store. Not enforced on a filesystem that cannot lock (some network filesystems), and the Windows side relies on the share mode and is covered only by the smoke test.

## Opening stores

- **A driver built with URI filenames off cannot open a branch unless you turn them on.** forkdb_open_db returns file:/forkdb/b<N>?vfs=forkdb, a URI. Python sqlite3 (uri=True) and node:sqlite pass the URI flag themselves. better-sqlite3 is built with SQLITE_USE_URI=0: set the environment variable SQLITE_USE_URI=1 before require(better-sqlite3), or its open fails with the unhelpful unable to open database file. (Until the shape file:/forkdb/b<N> the same mistake silently opened a plain file named file:b<N>?vfs=forkdb in the working directory and wrote into it.) Measured with better-sqlite3 13.0.3. Other drivers built the same way behave the same; untested.
- **A store whose first creation was interrupted by a power cut may be refused.** Opening treats a file shorter than the header (or exactly header-sized without a valid header) as a creation that was cut short only if every byte is zero or the byte our header has there; anything else is refused with an error and left alone, so that a file that is not a store (a 100-byte notes file, a one-page SQLite database) is never overwritten. A real interrupted creation leaves nothing, zeros or a prefix of the header and is recreated; the power-loss model also allows garbage there, and such a file is refused. No data can be in it: delete it and create the store again.

## File system requirements

- **Compaction on Windows needs NTFS on Windows 10 1709 or later.** It replaces the log with a rename that has POSIX semantics (`FileRenameInfoEx`), because the classic `MoveFileExW` cannot replace a file that has any open handle (`ERROR_ACCESS_DENIED`, measured). Where that rename is refused, compaction fails with an error before the replacement and the store is untouched and usable, but never shrinks. Until this was found compaction failed on every Windows machine: no test had ever compacted there. The directory sync after the swap flushes the directory if Windows allows it and the store file's handle otherwise, which is the best Windows offers and has not been tested against a power cut.
- **Compaction makes a new file.** The mode and the lock are carried over (both tested: `test/file_test.c`, `test/lock_smoke.c`), and the owner and group where the process is allowed to set them (not tested); extended attributes and ACLs are not; a second hard link to the store keeps the old content (not tested); and the store's path is resolved when it is opened (symbolic links followed on Unix), so a later `chdir()` does not matter.
- **Rename over an open file** is needed for compaction (`fk_compact`, open-time checkpoint). This works on local Unix file systems and WSL2. Without it, the store works but cannot compact — and the log grows with every fork + discard cycle.
- **On macOS a durable store calls `fsync`, not `F_FULLFSYNC`.** `fsync` on macOS does not ask the drive to flush its own write cache; `F_FULLFSYNC` does, and is what SQLite's `fullfsync` pragma uses. forkdb never calls it, so on macOS the claim that a returned commit survives a power cut rests on a weaker call than the simulator's model assumes. Not measured: what `F_FULLFSYNC` would cost; not changed.
- **A store opened with `nosync` is not power-loss safe.** It survives the death of the process; after a power cut it refuses to open or opens to a state its workload had acknowledged, usually an older one (see DURABLE-STORE.md). Not tested on a real power cut.
- **`fsync` semantics.** The store calls `fsync` after every commit unit and after recovery truncation. A file system that does not honour `fsync` (network mounts with write-back caching) can lose committed data on power loss.

## Platforms

- **The README's table says what runs where.** Windows needs mingw-w64 gcc to build the extension (`scripts/build-ext.sh` does not drive MSVC); macOS needs a compiler that takes gcc-style flags (Apple clang is what CI uses). 32-bit builds are untested.
- **The 3.24 minimum is a stated minimum.** The extension refuses to load below SQLite 3.24 with a message (tested against a 3.23.1 shell), and works in a 3.24.0 shell (tested: init, attach, fork, edit, integrity check, snapshot, discard, reopen from a second process). With the guard lowered in a scratch copy, the same scenario also worked in 3.23.1, so the number is a policy, not a measured crash. Nothing older than 3.23.1 was tried.
- **The library pins itself** so that closing the connection that loaded it does not unload it under the others: `dlopen(RTLD_NODELETE)` on Linux and macOS, `GetModuleHandleEx` with the pin flag on Windows. Without it a Windows run segfaulted at the first test that closes the loader (CI, first run); the Node loading tests cover it on all three.
- **The full Python suite is Linux-only** (the smoke test of the installed wheel runs on all three platforms), and so is the cross-check of `forkdb_sync_count()` against the `fsync` system calls (an `LD_PRELOAD` interposer). The `nosync` behaviour tests for both Node hosts run on all three platforms.

## Snapshots

- Snapshots can be up to 1 GiB of metadata stream (`FK_MAX_SNAP`). This is the serialised layer graph, not the page data.
- A snapshot is only accepted as the first unit of a file (version ≥ 2). A snapshot that does not pass its checksum is treated as corruption and the valid prefix before it is recovered instead.

## Branch ids

- Maximum `FK_MAX_BRANCHES` (default 1,024) alive at once. A fork returns `FK_ELIMIT` when all ids are in use.
- Ids are reused (format ≥ 3) after their branch is discarded. **The caller must drop its handle when it discards a branch**, because a later fork may be given the same number. This is a mitigation (next-fit search), not a guarantee.

## State ids

- State ids mean "same content" within one store: two live branches with the same state id hold equal content. The converse does not hold: a commit that writes back the same bytes gets a new id.
- State ids do not survive across stores. A compacted version 3 file gives every branch an id of its own, so branches that were equal are no longer recognised.

## No bundled SQLite

Only diff, promote and undo need the SQLite session API. The portable extension is built without it and reports `forkdb_info.session_api = 0` (in all three hosts it has run in); those three functions then return `FKX_ENOSESSION`. Branching, forks, snapshots, discard, reset and compaction need none of it. The library and the CLI are built with it; see SESSION-DESIGN.md for the host survey.

The CLI bundles its own session-variant SQLite — it works offline on any store.

## Test coverage

- The store and VFS each have power-loss test suites (10 seeds × 12+ operations with crash injection at every I/O event) plus over a dozen targeted mutants each.
- The fkx layer has a logic pass (111 checks), a power-loss pass (session driver: 10 seeds × 20 operations covering both replay and fast-forward), and 20 mutants — 10 core, 10 session — each caught by at least one test variant.
- The CLI and extension have smoke tests that exercise every operation end-to-end. The extension test loads via `sqlite3_load_extension`.
- Node: the extension is tested in node:sqlite (Node 24, `scripts/test-ext-node.sh`: loading, branches, locking, nosync, worker threads). better-sqlite3 13.0.3 is tested too (`scripts/test-ext-bsq.sh`: loading, branches, locking, worker threads, nosync; it needs `SQLITE_USE_URI=1`, see above). Both Node suites also run on macOS and Windows in CI, without the nosync files. Not tested: other Node bindings, other better-sqlite3 versions, Node versions other than 24. node:sqlite reads SQL integers as JavaScript numbers and throws above 2^53, which is why new branch ids are 53 bits (this was found by that suite).
- **macOS has been tested only in CI**, on GitHub's macOS runner (an arm64 virtual machine), and never by hand on a real Mac; the macOS wheel and CLI have been run nowhere else. A problem that shows only on real hardware, on another macOS version or on Intel would not have been seen. (Windows was checked once by hand on Windows 11, as well as in CI.)
- Untested: OOM injection, virtual table handling at the SQL level, and the full Python suite on macOS and Windows (only the smoke test of the installed wheel runs there). On macOS the Python that GitHub's `actions/setup-python` installs (3.12.10, SQLite 3.49.1) has no `Connection.enable_load_extension` and cannot load forkdb; Homebrew's Python (3.15.0, SQLite 3.54.0) can, and a second `python3` on the runner's PATH could not either. python.org's macOS installer is untested.
