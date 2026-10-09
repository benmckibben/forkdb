# forkdb

Instant-branching, copy-on-write SQLite databases for local development, testing and agent sandboxing: fork a database in microseconds, let a test or an agent do anything to the fork, then look at the diff and promote it or throw it away.

**Status: just started.** The form factor is decided and a feasibility spike has validated the core mechanics. There is **no product code yet**: nothing here is installable.

## The idea

- A **branch** is an immutable base plus a small overlay of changed pages, exposed to SQLite through a custom VFS. Forking is O(1) and costs a few dozen bytes however large the database is.
- A **logical layer** (SQLite's session extension) records what a branch changed as a changeset, which gives diff, promote ("fast-forward or replay with conflict report") and undo.
- Target uses: an agent that gets a throwaway copy to try a risky migration or destructive task in; one database per test in milliseconds; "what would this change do?" with a reviewable diff.

## Where things are

| | |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | Why SQLite VFS (and not an embedded KV store or a proxy over Postgres/MySQL), the planned v0.1 scope, what is deliberately left out |
| [docs/SPIKE-FINDINGS.md](docs/SPIKE-FINDINGS.md) | What the spike proved, measured, and what it did not show |
| [spike/](spike/README.md) | The throwaway spike: one C file, build and run instructions, recorded results |

## Reproduce the spike

```sh
spike/build.sh batch && spike/build/spike-batch
```

Needs gcc, curl, sha256sum and python3 (the first build downloads and compiles a pinned SQLite, which takes a few minutes).

## Known facts that shape the project

- Batch-atomic writes need a **custom SQLite build**, so forkdb has to be correct without them.
- In WAL mode a fork must carry the WAL or checkpoint first.
- Changesets do **not** capture tables without a PRIMARY KEY or schema changes, so promote must check the schema itself.

## License

Licensed under either of [Apache License, Version 2.0](LICENSE-APACHE) or
[MIT license](LICENSE-MIT) at your option.
