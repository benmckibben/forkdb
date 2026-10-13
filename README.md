# forkdb

Instant-branching, copy-on-write SQLite databases for local development, testing and agent sandboxing: fork a database without copying it, let a test or an agent do anything to the fork, then look at the diff and promote it or throw it away.

**Status: just started.** The form factor is decided, a feasibility spike has validated the core mechanics, and the durable page store underneath is built and power-loss tested on its own. A second spike ([vfs/](vfs)) connects the store to SQLite through a VFS and passes the same kind of power-loss tests. It is single-process, has no WAL and no diff or promote yet: nothing here is installable.

## The idea

- A **branch** is an immutable base plus a small overlay of changed pages, exposed to SQLite through a custom VFS. Forking does not copy any pages: it costs one small log record and one small in-memory layer however large the database is (the spike measured about 48 bytes and 14-18 ns for the in-memory prototype; the durable store adds an fsync per fork).
- A **logical layer** (SQLite's session extension) records what a branch changed as a changeset, which gives diff, promote ("fast-forward or replay with conflict report") and undo.
- Target uses: an agent that gets a throwaway copy to try a risky migration or destructive task in; one database per test in milliseconds; "what would this change do?" with a reviewable diff.

## Where things are

| | |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | Why SQLite VFS (and not an embedded KV store or a proxy over Postgres/MySQL), the planned v0.1 scope, what is deliberately left out |
| [docs/SPIKE-FINDINGS.md](docs/SPIKE-FINDINGS.md) | What the spike proved, measured, and what it did not show |
| [spike/](spike/README.md) | The throwaway spike: one C file, build and run instructions, recorded results |
| [docs/VFS-SPIKE.md](docs/VFS-SPIKE.md) | SQLite running on the durable store through a VFS ([vfs/](vfs)): what was built, the tests and mutant check, the measurements, and what it changed in the plan |
| [docs/DURABLE-STORE.md](docs/DURABLE-STORE.md) | The durable, branchable page store ([store/](store)): format, recovery, the power-loss test harness and what it does not cover; and an opt-in layer that preallocates the log (about 3 times cheaper commits on real disks); format version 4: branch metadata, reset and state ids for the layer above |
| [docs/SESSION-DESIGN.md](docs/SESSION-DESIGN.md) | Design note for diff, promote and undo on the VFS through SQLite's session extension: what was measured, the store changes it needs, the schema policy, packaging. Design only, no code yet |
| [docs/FKX-DESIGN.md](docs/FKX-DESIGN.md) | The branch layer: names, fork with a base, snapshot, reset, discard, fast-forward promote, undo, garbage collection, the metadata format. Design and what is implemented (the core: [fkx/](fkx/); the session driver for diff and replay is not yet). Tests: [scripts/test-fkx.sh](scripts/test-fkx.sh) |

## Reproduce the spike

```sh
spike/build.sh batch && spike/build/spike-batch
```

Needs gcc, curl, sha256sum and python3 (the first build downloads and compiles a pinned SQLite, which takes a few minutes).

## Known facts that shape the project

- Batch-atomic writes need a **custom SQLite build**, and forkdb does not use them: the VFS spike measured the same commit cost with and without ([docs/VFS-SPIKE.md](docs/VFS-SPIKE.md)), so they are out of the design.
- In WAL mode a fork must carry the WAL or checkpoint first.
- Changesets do **not** capture tables without a PRIMARY KEY or schema changes, so promote must check the schema itself.

## License

Licensed under either of [Apache License, Version 2.0](LICENSE-APACHE) or
[MIT license](LICENSE-MIT) at your option.
