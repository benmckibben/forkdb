# forkdb spike: findings (throwaway; 2026-10-09)

Setup: SQLite 3.50.0 amalgamation, one C file ([spike/spike.c](../spike/spike.c), about 320 lines) implementing an **in-memory VFS ("layerfs")** whose main-db file is a chain of copy-on-write page layers (4096-byte pages), with rollback-journal/WAL files as memory buffers, xShm* for WAL, and fault injection. Built with `-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK` (plus `-DSQLITE_ENABLE_BATCH_ATOMIC_WRITE` for the second run). Results: [spike/results/out-batch-atomic.txt](../spike/results/out-batch-atomic.txt) (the full final run) and [spike/results/out-stock.txt](../spike/results/out-stock.txt) (an earlier stock-flag run, before the core-fork microbenchmark was added). **All checks pass** in the batch-atomic run (TOTAL FAILS=0); in the stock run the only failure was the batch-atomic check, which is explained by finding 2. Reproduce with [spike/README.md](../spike/README.md).

## Objective 1: VFS vs WAL / rollback / batch-atomic

1. **Rollback modes all work** on the layered VFS: DELETE, TRUNCATE, PERSIST, MEMORY and OFF; for each, 1000 rows, integrity_check ok, and ROLLBACK restores the rows (not tested for OFF, whose rollback is undefined by SQLite).
2. **Batch-atomic write is NOT available in stock SQLite.** The VFS flag `SQLITE_IOCAP_BATCH_ATOMIC` has no effect unless SQLite is compiled with `-DSQLITE_ENABLE_BATCH_ATOMIC_WRITE` (it is also incompatible with `SQLITE_MMAP_READWRITE`). Without it, `BEGIN_ATOMIC_WRITE` was never called (begin=0). So an extension loaded into a stock `sqlite3` (system library, Python, most language drivers) gets the normal journal path; forkdb has to work correctly **without** it and treat batch-atomic as an optimisation that needs a custom SQLite build.
3. **With the flag**, it works and is limited as expected:
   - A 10-row txn: 1 begin, 1 commit, 0 journal files.
   - 5000 rows (about 600 KB of dirty pages) at a 2000 KB page cache: one batch of 138 pages, no journal.
   - The same txn at 256 KB, 64 KB and 16 KB caches: no batch at all, the pager falls back to the normal journal (journal-opens=1). **The size limit is "dirty pages must fit in the page cache"**, not a fixed constant.
   - A failing `COMMIT_ATOMIC_WRITE`: begin=1, commit=0, rollback=1, then SQLite **retried through the rollback journal and the transaction succeeded** (rows 20010 to 20020, integrity ok). So a VFS can refuse a batch safely.
4. **WAL works through a purely in-memory xShm** (regions, shared/exclusive slot locks, barrier, unmap). A second connection sees a consistent snapshot while a write txn is open and sees the commit afterwards.
5. **The sharp edge is forking in WAL mode.** After 300 tiny autocommit transactions the db file was still 4096 bytes while the `-wal` file was 2,488,512 bytes. Forking only the main-file layer gave a fork with **no data at all** ("no such table"). Two correct strategies were verified: (a) fork plus a **copy of the WAL** (the wal-index is rebuilt on open; fork cost is then proportional to the WAL size, not O(1)); (b) `PRAGMA wal_checkpoint(TRUNCATE)` first (WAL goes to 0 bytes), then an O(1) fork with no WAL copy. Rule for forkdb: **a snapshot is main file + WAL, or checkpoint first**.

## Objective 2: layered page table

- **O(1) fork confirmed.** The core operation (freeze the head layer, allocate one empty overlay) costs **about 14-18 ns and 48 bytes**, identical for 1 MB (252 pages), 16 MB (4011 pages) and 64 MB (16041 pages). The named-fork figures in the output (8-18 microseconds) include **this spike's linear name table scan** over up to 8192 branches and are not a property of the design.
- **Copy-on-write granularity is the page.** A one-row UPDATE on a fork of the 16 MB db dirtied **2 of 4011 pages** (8 KB); a 18-row-change workload dirtied 4 pages (16 KB) where its logical changeset was 364 bytes.
- **Isolation both ways**: a write on the fork is invisible in the base; a write on the base **after** the fork does not leak into the fork (snapshot semantics). Open connections keep working across a fork because the layer pointer is looked up on every I/O.
- **Read cost grows with chain depth.** A full scan of the 16 MB db: 1.4 ms through 2 layers, 2.4 ms through 101, 10.6 ms through 401. Long chains need **flattening/compaction** (and a per-layer index instead of walking parents) before they matter; for realistic fork depth (single digits) the cost is negligible.
- Tombstones are needed for truncation (a layer must hide parent pages beyond its size); implemented and not hit by a failing test, but only lightly exercised (VACUUM and page-count shrink are not tested).

## Objective 3: session extension (logical diff)

- **Changeset capture works** on a fork: 18 row changes (10 inserts, 5 updates, 3 deletes) plus others gave a **364-byte, 19-operation changeset**, versus 4 dirty pages (16 KB) at the page level. The logical layer is the right place for diff, review, replay and undo.
- **Promote (fast-forward)**: applying the fork's changeset to the unchanged parent reproduced table a **exactly** (rc=0, 0 conflicts).
- **Diverged merge**: fork 1 changed ids 1-5, fork 2 changed ids 3-8; applying 1 onto 2 reported **exactly 3 conflicts** (ids 3-5), and with a REPLACE handler non-overlapping rows merged correctly.
- **Undo**: `sqlite3changeset_invert` applied to the fork returned it to the original state **exactly** (a cheap "reset to snapshot" for logical state).
- **Sharp edges verified, not assumed:**
  - **Tables without a PRIMARY KEY are not tracked** (the parent kept 5 rows in such a table, the fork had 6).
  - **DDL is not captured** (a CREATE TABLE made on the fork did not exist in the parent after the apply).
  - A row insert into a table created after the session attached **was** recorded as an operation (it counts in the 19) but the table is missing in the parent; no conflict was reported (rc=0). forkdb must detect schema drift itself (compare `sqlite_schema` or the schema cookie) instead of trusting apply's return code.

## Failure modes: crash and fault injection

Torn write at I/O step N, after which all I/O fails (journal, WAL and db writes, syncs, truncates and deletes all counted), then reopen and verify, **at every step**:

| Mode | I/O steps | Recovered old | Recovered new | Violations (corruption or partial state) |
|---|---|---|---|---|
| rollback journal (DELETE) | 15 | 14 | 1 | **0** |
| WAL | 14 | 4 | 10 | **0** |
| batch-atomic | 5 | 3 | 2 | **0** |

Batch-atomic shrinks the crash window from 15 steps to 5 because there is no journal. All three recover to either the old or the new state with `integrity_check` ok.

## What this spike does NOT show (do not over-read it)

- **Memory only.** No persistence: layers live in RAM. A durable layer store (append-only file with checksummed page records, a manifest, atomic manifest switch) is the next thing to build, and is where durability bugs will live.
- **Crash model is limited.** Torn write then total failure. It does **not** model lost un-fsynced writes or write reordering after power loss; that needs a proper "drop random unsynced writes" harness before any durability claim.
- **Single process.** Locks are simplified and in-process. A loadable extension in a multi-process setting needs real file locking and shared memory for the WAL index.
- **No garbage collection** of unreachable layers, no flattening, no VACUUM/page-size/auto_vacuum handling, no huge-transaction or concurrency stress.
- **Compile flags matter**: session needs `SQLITE_ENABLE_SESSION`+`SQLITE_ENABLE_PREUPDATE_HOOK`; batch-atomic needs its own flag. Stock distro SQLite (and Python's) may lack the session extension too (Python's 3.45.1 stdlib was not checked for it).
- Performance numbers are single-run, one machine, `-O1/-O2`, and were not repeated.

## Implications for the v0.1 design (Option A holds)

1. **Keep page layers for branching and the session extension for diff/promote/undo**; both are validated.
2. **Ship a pinned SQLite build** (amalgamation compiled with the right flags) rather than depending on a system library; make batch-atomic optional.
3. **Snapshot = main file + WAL (or checkpoint first).** Make `fork` checkpoint by default.
4. **Promote must be schema-aware**: refuse when `sqlite_schema` differs; warn or refuse tables without a PRIMARY KEY.
5. **Plan for layer flattening/compaction** from the start.
6. **Next steps if approved:** a durable layer store plus a power-loss harness; then the CLI (`init/fork/diff/promote/discard`); then the loadable-extension packaging question.
