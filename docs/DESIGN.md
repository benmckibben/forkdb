# Design decision: SQLite VFS with copy-on-write page layers

Status: **decided** (2026-10-09), v0.1 not started. This records the choice among three candidate form factors and the reasoning, so it can be challenged later. The analysis is from reading and reasoning; only the parts listed in [SPIKE-FINDINGS.md](SPIKE-FINDINGS.md) were measured.

## The three candidates

### A. SQLite VFS with copy-on-write layers (chosen)

A branch is an immutable base plus an overlay of dirty pages, served to SQLite through a custom VFS. A fork freezes the current overlay and starts a new empty one. A snapshot is a frozen layer.

- **Feasibility: high, complexity moderate.** The VFS interface is small and stable. Page lookup through a chain of layers is simple; flattening is needed for long chains.
- **Diff and merge** cannot be done on pages (two branches that touch the same page may touch different rows). They are done on a logical layer with SQLite's session extension (changesets).
- **Sharp edges:** WAL needs shared memory and real locking; transactions that don't fit the batch-atomic path need a journal fallback; the database is only visible to programs that open it through the VFS (so it needs to ship as a loadable extension plus a library); fsync ordering mistakes are the classic way to lose data; VACUUM rewrites every page and bloats a layer chain; layers need garbage collection.
- **Why it fits the use:** SQLite is what agents and local tools already use; it is in-process, with no server to manage; and agent sandboxing (fork, run something destructive, review the diff, promote or discard) is the sharpest need.

### B. Embedded key-value store with Git-like DAG branching

A persistent copy-on-write tree (a prolly tree as in Dolt, or an LMDB-style tree with several roots) gives O(1) branches and clean three-way merge.

- **The cleanest model on paper, the most work in practice:** it means writing a storage engine (crash safety, concurrent writers, range scans, chunk GC, compaction, write amplification).
- **No SQL.** Few people reach for a bare KV store to hold their app's data in development; adoption would depend on a query layer that would also have to be built.
- **Crowded:** Dolt, Irmin and lakeFS already own "Git for data".
- Rejected for v0.1.

### C. Dev proxy or wrapper over real databases (Postgres, MySQL)

A local layer that manages ephemeral clones of a real database server.

- **Strength:** full compatibility; unmodified applications connect.
- **"Instant" depends on the filesystem.** Cloning is near-instant on APFS, btrfs, ZFS and reflink-enabled XFS, and silently becomes a full copy on ext4 and many Docker volumes. A tool that demos well on a Mac and is slow on the Linux CI machine earns a bad reputation.
- Each fork is a running server (memory, port), a live data directory must be quiesced or snapshotted atomically before it is cloned, and there is a version and extension matrix to support.
- **Crowded:** Neon, Xata, PlanetScale, Supabase branching and DBLab cover it, and PostgreSQL 18's reflink-based file copy option lets people do much of it themselves (that last point is from a search summary I did not verify in the PostgreSQL docs).
- Possible later as an additional backend; not the v0.1 form factor.

## Comparison

| | A: SQLite VFS | B: embedded KV | C: dev proxy |
|---|---|---|---|
| Setup | one extension or library, no daemon | library plus a query layer | daemon plus a DB server per fork |
| App changes | open through the VFS | rewrite to the KV API | none |
| Fork speed | O(1) everywhere | O(1) everywhere | instant only on CoW filesystems |
| Best at | agent sandboxes, per-test databases, risky migrations | niche | teams already on Postgres |
| Main trap | other tools can't see the database unless it is a loadable extension | hard to say what it is for | slow forks on ext4 and Docker volumes |

## v0.1 scope

In:
- A layered, copy-on-write page store behind a SQLite VFS, shipped as a loadable extension plus a small library and CLI.
- Operations: init, fork, snapshot, diff, reset, discard, promote.
- Diff, promote and undo through session changesets. Promote fast-forwards when the parent has not moved, otherwise replays the changeset and reports conflicts.
- The contract: one process, many connections, documented journal modes.
- Crash and power-loss testing as part of the release, not after it.

Out:
- Cross-process access, remote storage, multi-branch merge graphs, anything for Postgres or MySQL.

## Consequences of the spike

1. Ship a **pinned SQLite build** with the right compile flags; make batch-atomic an optional optimisation. *Update 2026-10-11: the VFS spike over the durable store found batch-atomic gives no benefit there (the store's commit is already atomic, so the journal can stay in memory); see [VFS-SPIKE.md](VFS-SPIKE.md). A pinned build is still needed if the session extension is required, which that spike did not test.*
2. A snapshot is main file plus WAL; **fork checkpoints by default**.
3. **Promote must be schema-aware**: refuse when the schema differs, warn or refuse for tables without a PRIMARY KEY.
4. Plan **layer flattening** from the start.
5. The next thing to build is a **durable layer store** (append-only file with checksummed page records, a manifest, an atomic manifest switch) together with a **power-loss test harness** that drops un-synced writes; the spike's crash model (torn write then total failure) does not cover that.

## Open questions

- Packaging: a loadable extension still runs inside whatever SQLite the host program links, which may lack the session extension and batch-atomic support. Does forkdb ship its own SQLite (simplest, but a second copy of SQLite in the process) or require a capable one?
- Language for the real implementation (C for the VFS is natural; the CLI and library bindings could be Rust or Go).
- ~~License~~ Decided: MIT OR Apache-2.0, as in the author's other projects.
