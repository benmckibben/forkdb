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

- **Single-writer.** One writer transaction at a time, across all branches, enforced by the VFS refusing the super-journal. Reads from separate connections on different branches are fine. The store is thread-safe only through the VFS and fkx, which take one mutex around everything: a connection per thread is fine, but threads on different branches take turns inside the VFS (a commit, a read of a page) rather than run in parallel. Used directly (fkstore.h) the store is not thread-safe.
- **One process per store file.** The file is locked while open; a second process (the CLI while an application has the store open, two workers on one path) gets an error, not a damaged store. Not enforced on a filesystem that cannot lock (some network filesystems), and the Windows side relies on the share mode and is covered only by the smoke test.
- **One writer per branch.** The VFS enforces this: a transaction on one branch and another on a different branch in the same connection opens a super-journal, which the VFS refuses (with a reason logged). Separate connections each on their own branch, reading, is fine.

## File system requirements

- **Rename over an open file** is needed for compaction (`fk_compact`, open-time checkpoint). This works on local Unix file systems and WSL2. Without it, the store works but cannot compact — and the log grows with every fork + discard cycle.
- **A store opened with `nosync` is not power-loss safe.** It survives the death of the process; after a power cut it refuses to open or opens to a state its workload had acknowledged, usually an older one (see DURABLE-STORE.md). Not tested on a real power cut.
- **`fsync` semantics.** The store calls `fsync` after every commit unit and after recovery truncation. A file system that does not honour `fsync` (network mounts with write-back caching) can lose committed data on power loss.

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

The library and extension need a host that exposes the session API. The extension reports whether it found one through `forkdb_info.session_api`. Without it, diff, promote and undo return `FKX_ENOSESSION`.

The CLI bundles its own session-variant SQLite — it works offline on any store.

## Test coverage

- The store and VFS each have power-loss test suites (10 seeds × 12+ operations with crash injection at every I/O event) plus over a dozen targeted mutants each.
- The fkx layer has a logic pass (111 checks), a power-loss pass (session driver: 10 seeds × 20 operations covering both replay and fast-forward), and 20 mutants — 10 core, 10 session — each caught by at least one test variant.
- The CLI and extension have smoke tests that exercise every operation end-to-end. The extension test loads via `sqlite3_load_extension`.
- Untested: OOM injection, virtual table handling at the SQL level, Node.js host primitives, and the host survey (macOS, python.org SQLite, Windows).
