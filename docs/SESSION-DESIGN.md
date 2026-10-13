# Session extension on the VFS: design note

Status: **design, decided by Ben on 2026-10-12; steps 1 (the session build, the one-branch-write contract and the host survey) and 2 (store format v4) of the build plan are implemented, the rest is not.** What was measured is in [vfs/results/session-experiments.txt](../vfs/results/session-experiments.txt) (produced by [vfs/experiments/session-experiments.sh](../vfs/experiments/session-experiments.sh), a throwaway program that drives SQLite's session extension through the real VFS and a real file store). Where a statement below is reasoned and not measured, it says so.

The goal ([DESIGN.md](DESIGN.md), v0.1 scope): **diff, promote and undo** for branches, through SQLite session changesets. Promote fast-forwards when the parent has not moved, and otherwise replays the changeset and reports conflicts. The three open questions were: what happens when a transaction touches two attached branches, what schema drift does to a replay, and how to get a SQLite with the session extension.

## Summary

1. **Diff works, as a comparison after the fact.** `ATTACH` the fork's base, attach a session to the fork, call `sqlite3session_diff`: no recording while the application writes, so it works for any connection, any process lifetime and after a crash. It costs a scan of each table: 598 ms for a 77 MiB table (about 9 times a plain scan of it), 61 ms for 8 MiB.
2. **Replay needs the state the fork started from, kept as a branch.** A fork is therefore two branches: a read-only **base** (a snapshot of the parent at fork time) and the fork. Nothing else records the fork point.
3. **The store needs three small additions (format v4):** a `RESET` record (a branch takes another's state in O(1): fast-forward promote, undo, and the *reset* operation already in the v0.1 list), a small per-branch **metadata** blob (names, kinds, relations, written atomically with the fork), and a per-branch **state id** (to know the parent has not moved). Without the last one, fast-forward cannot be told safely: SQLite's own change counter can repeat after a reset (reasoned, see below).
4. **Promote is one SQLite transaction on the parent** (so one store commit and atomic) when it replays, or one store record when it fast-forwards. It refuses what a changeset cannot carry (see the policy table): SQLite silently skipped or misapplied three of the eight schema-drift cases tried (two more were caught only because the data happened to differ).
5. **History is snapshots, not stored changesets.** An undo point is a pair of snapshots around a promote; the inverse is computed when needed. Nothing is written into the user's database, and no reserved tables are needed.
6. **A transaction that writes two attached branches was not atomic: it committed as two separate durable store commits.** Measured. Step 1 makes the VFS refuse it (the commit fails with `SQLITE_CANTOPEN`, nothing is published), **but only where SQLite tells the VFS**: in the default journal modes with a branch as the main database. The two other cases (journal modes MEMORY and OFF, and an in-memory or temp main database) are in the section below. Reads across branches are fine.
7. **Packaging (decided):** the amalgamation's `sqlite3.h` does not declare the session API, and the extension interface (`sqlite3ext.h`) has no session functions, so a loadable extension cannot call them through the host. **forkdb does not put a second SQLite in the application's process.** The in-app library/extension gives the VFS to any host and gives diff/promote/undo only to hosts that expose the session API, and says so plainly when one does not. Only the standalone CLI, a separate program that works on a store no application has open, is built with its own SQLite with the two flags.

## What the experiments showed

| Question | Answer | Experiment |
|---|---|---|
| Can a fork be diffed against its parent in one connection? | Yes: `ATTACH 'b0' AS base`, a session on `main`, `sqlite3session_diff(ss, "base", table)`. 16 changes on a test fork, all correct. | X1 |
| Tables the session cannot diff | A table without a PRIMARY KEY: **no error, no changes** (the rows inserted into it are missing from the changeset). A table that is not in the base: `SQLITE_SCHEMA`, "no such table: base.t4". | X1 |
| Does DDL travel in a changeset? | **No.** A new table and an added column are not recorded; only row changes are (the rows of the new table are). | X4 |
| Two attached branches, one write transaction | **Commits, as two store commits** (`sync_publish` +2, `store_commits` +2). A power cut between them leaves one branch changed and the other not. | X2 |
| ...with a VFS that refuses super-journal files | `COMMIT` fails with `SQLITE_CANTOPEN`, 0 store commits, neither branch changed. | X2 (refuse) |
| Read a branch and its base consistently | A read transaction holds the read lock on both; a writer on the other branch gets `SQLITE_BUSY` until it ends. Single-threaded, so one statement cannot be interleaved anyway. | X3 |
| Apply a changeset when the target's schema moved | See the policy table: skipped silently (table dropped, primary key changed), applied by position (column renamed), applied (column added at the end, NOT NULL and CHECK added), conflict (type changed, column inserted before). | X4 |
| Triggers | **They fire during apply**, so rows a trigger wrote are inserted twice (the changeset carries them too): a CONFLICT and an abort. Dropping the triggers inside the transaction, applying with `SQLITE_CHANGESETAPPLY_NOSAVEPOINT` and re-creating them gave the right rows and a working trigger. | X5, X5b |
| Foreign keys | With `foreign_keys=ON` a child row whose parent is gone raises a FOREIGN_KEY conflict (rc 19) and aborts. With OFF the **orphan is applied silently.** | X5c |
| Replay, parent moved on other rows | Applies; the parent's other changes are kept. | X6 (a) |
| Undo by inverting the changeset | Restores the exact aggregate (100 rows, 290 characters) and removes the inserted row. | X6 (b) |
| Parent changed the same row | DATA conflict; the handler answering ABORT leaves the parent untouched (no transaction left open). Same primary key inserted on both sides: CONFLICT. Answering OMIT skips that change and applies the rest. | X6 (c) to (e) |
| Can an extension call the session API? | `sqlite3ext.h` has no session function (0 matches). An extension linked against the host's `libsqlite3.so.0` ran it in Python's SQLite (3.45.1 here, which has both compile options). | packaging |
| Cost of the session build | The two compile options with no session attached: no difference outside noise. A session attached to every table: see Cost. | bench |

## Design

### The fork point is a branch

A promote that cannot fast-forward needs *what the fork changed*, which is the difference between the fork and the state it started from. The parent has moved on, so that state exists nowhere else. Two ways to keep it were considered:

- **A base branch (chosen).** Forking P creates a read-only snapshot A of P, then F as a fork of A. A is an ordinary branch, so SQLite can `ATTACH` it by name, and it costs one small record. It uses a branch id (FK_MAX_BRANCHES is 1024 alive at once, so a fork costs two) and pins the pages only A can see, which is the same pages F pins while it lives; discarding F discards A.
- **A fork-point view inside the store** (the layer F was built on is already there). Rejected: the store frees and merges layers as branches are discarded (`store/fkstore.h`), so it would need an explicit pin in the format anyway, and SQLite could not attach it without a new kind of VFS name.

Base branches must be read-only, or one stray write corrupts every later diff. The VFS will refuse a read-write open of a branch whose metadata says so.

### Store additions (format version 4)

| Addition | Why |
|---|---|
| `RESET dst src` record (O(1), one fsync) | Fast-forward promote (P becomes F), undo to a snapshot, and the *reset* operation of the v0.1 list. |
| Per-branch metadata, at most a few hundred bytes, last writer wins, optionally part of the FORK record | Branch names, kind (fork, base, undo point), origin, ids of undo points. A fork and its label are one atomic record. Survives compaction. |
| Per-branch **state id**: copied by fork **and by reset**, replaced by every commit (a store-wide counter, not the log sequence number, which starts again after a compaction) | "P has not moved since A was taken" is `state(P) == state(A)`. |
| ~~`fk_branch_parent`~~ | **Not built.** Branch ids are reused, so a stored parent id could silently name a different branch later. The layer above records the origin in the metadata. |

**Why not use SQLite's file change counter for the state id** (page 1, bytes 24 to 27, which every write transaction increments): it would need no format change. It is not safe once a branch can be reset: P reset to an older snapshot and then advanced reaches the same counter value with different content, and a fast-forward decided on equal counters would throw P's content away. This is reasoned from how the counter works, not tested.

**A reset under an open connection is a hazard** (reasoned, to be tested when it is built): SQLite keeps a page cache per connection and decides whether it is stale by that same counter. After a reset to content with an equal counter, a warm connection would serve old pages. v0.1: the VFS refuses a reset (so a fast-forward promote and an undo to a snapshot) while any connection is open on the target, which it can see (`Branch.nref`), with `SQLITE_BUSY`. The replay paths are ordinary write transactions and do not have this limit.

A version 3 file keeps working as it is; the first v4 feature needs an upgrade, which is a compaction (already crash-safe), not an in-place header change. Older programs refuse a version they do not know, as they do today.

### Diff

`diff(F)` is F against its base; `diff(X, Y)` is any two branches. One connection on X, `ATTACH` Y, `BEGIN` (so both read locks are held and a writer gets BUSY), one session on `main`, `sqlite3session_attach` and `sqlite3session_diff` for each table with a primary key present in both. Output: a changeset (and a per-table summary of inserts, updates and deletes). It lists the tables it could not diff (no primary key, or missing on one side) instead of leaving them out silently, because the library does.

Cost is O(size of the tables), not of the change: 5 ms (1 MiB), 61 ms (8 MiB), 598 ms (77 MiB) for 100 changed rows, about nine times a scan of the same table. A page-level shortcut is possible later (the store knows which pages a branch rewrote since its base, and a table whose pages did not change need not be compared) but needs b-tree parsing and a new store call; not designed here.

### Promote

Promote F into P (P is F's recorded origin unless the caller names another branch):

1. **Fast-forward** if `state(P) == state(base of F)`: take an undo point of P (below), then `RESET P := F`. Exact, O(1), carries any DDL, needs no connection open on P.
2. **Otherwise replay**, only if the policy table allows it, in this order: take the undo point; compute `diff(base, F)`; on a connection to P with `foreign_keys=ON` set *before* the transaction, `BEGIN IMMEDIATE`; check the touched tables' schema against the base; drop the triggers on the touched tables; `sqlite3changeset_apply_v2` with `SQLITE_CHANGESETAPPLY_NOSAVEPOINT` and a conflict handler that records the conflict and answers ABORT; re-create the triggers; `COMMIT`. **P changes only at that commit, which is one store commit.** A conflict, a failed check or a crash leaves P as it was.
3. `--dry-run` runs the same inside a transaction that is rolled back, with the handler answering OMIT so that *every* conflict is listed, not just the first.

Both paths change P in exactly one atomic step. A crash can only leave an unused undo point (garbage, found by the metadata), never a half-promoted P.

A second fork promoted after a first one replays against the moved P, so overlapping rows come back as DATA conflicts: this is the three-way merge, and it needs no extra mechanism. After a promote the fork's base is stale; re-basing a fork onto its parent is not designed here.

### Undo and history

An **undo point** of P is two snapshots: `before` (taken just ahead of the promote) and `after` (just behind it), both read-only branches named in P's metadata. **Undo(P)** applies `diff(after, before)` to P by the replay path, so work done on P since is kept and a conflict aborts it; or, if P is exactly where the promote left it (`state(P) == state(after)`) and nothing is open on it, `RESET P := before`, which is exact and O(1). No changeset is ever stored: they are recomputed from two snapshots, so nothing is written into the user's database and a schema never has to be reserved for us.

A crash between the promote and recording its undo point loses the ability to undo that one promote; it never loses data. Each undo point costs two branch ids and the pages only they can see, so history is bounded: keep the last N per target (default to be chosen; 8 would use 16 ids) and discard older ones. A garbage collection discards branches whose metadata says they belong to something that no longer exists.

### Schema policy for promote

| Situation | Fast-forward | Replay |
|---|---|---|
| The fork changed the schema (DDL; a changeset cannot carry it) | allowed | **refused**, with the schema difference shown |
| The parent changed the schema of a table the changeset touches (`PRAGMA table_xinfo` differs from the base: names, types, NOT NULL, primary key, hidden) | n/a | **refused**. SQLite would skip the table (dropped, primary key changed) or apply it by position (renamed, inserted before) |
| The fork changed rows of a table without a PRIMARY KEY (found by comparing both ways with `EXCEPT`) | allowed | **refused**; the session extension cannot replay them and says nothing |
| Virtual tables, and AUTOINCREMENT counters (`sqlite_sequence` has no primary key) | allowed | refused for virtual tables; counters are not merged (documented) |
| The parent has triggers | n/a | allowed: dropped and re-created inside the transaction |
| Foreign keys | n/a | enforced (`foreign_keys=ON`); a violation aborts |
| Conflicts | n/a | abort and report; `--dry-run` lists them all |

The refusals are the product's honesty: a promote that succeeds must mean the parent now contains what the fork did.

### Transactions across branches

Contract (decided, option A; implemented in step 1): **a transaction writes one branch.** Enforcement has two parts and one limit that no VFS can remove.

- **The super-journal is refused.** When one transaction writes several attached database files, SQLite makes a super-journal; `xOpen` refuses it (`SQLITE_OPEN_SUPER_JOURNAL`), so the `COMMIT` fails with `SQLITE_CANTOPEN` (rc 14, "unable to open database file") before anything is published, SQLite rolls the transaction back, and the reason is sent to `sqlite3_log` ("fkvfs: a transaction may write only one branch..."; visible only if the host set a log callback) and counted in `FkVfsStats.super_journal_refused`. Test T14, in journal modes DELETE, TRUNCATE and PERSIST, with and without batch-atomic advertised: the commit fails, 0 store commits, the transaction is rolled back, neither row is there, the connection keeps working, reads of both branches in one transaction and a temp table with one branch still work.
- **Journal modes MEMORY and OFF are refused**, like WAL. **Found while implementing, and missed in my recommendation:** in those two modes SQLite makes no super-journal at all, so the refusal above never fires and the commit went through as two store commits (measured on the stock and the batch build). They are refused in `xFileControl` before SQLite acts on the pragma. This is a behaviour change (test T6 accepted them before). Cost: none that I can see, since the journal is already kept in memory on this VFS (reasoned, not measured).
- **Known limit: an in-memory or temporary main database.** When the connection's own main database is not a branch (`:memory:`, or a temp database) and branches are attached, SQLite commits all of them in one phase and makes no super-journal, so the VFS is never told. Two written branches commit as two store commits (measured; pinned by T14 as a known limit, so that a change in SQLite shows up). **No design that lives in the VFS can close it**, including committing several branches atomically (the rejected option C): the VFS never learns that the two writes belong together. Our own diff and promote open a branch as the main database and write only that one, so they are not affected; an application that attaches branches to an in-memory connection has to write one branch per transaction itself. This goes in the user documentation.

The roughness that remains: the failure comes at `COMMIT`, not at the first write, and the error text does not say why. Rejected: allowing it and documenting the crash window (it breaks "nothing partial after a crash"), and making several branches commit atomically in one store record (a format change and VFS work for which no feature has a use, and it would not close the limit above).

### Packaging

- The amalgamation's `sqlite3.h` **does not declare the session API**; the header is inside `sqlite3.c` between two marker comments (1,819 lines). The build extracts it (the experiment script does this) rather than vendoring a second download.
- The pinned SQLite 3.50.0 compiled with `-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK` and ran every experiment through the VFS. **Step 1 adds it as the `session` build variant of `vfs/build.sh`, extracts the header the same way, and runs the VFS gate on it** (test T15: diff through `ATTACH` finds exactly the work done, a replay on another fork is exactly one store commit, a second replay conflicts and publishes nothing, the inverse restores the base, and it survives a reopen).
- **Library (the in-app extension): no SQLite of ours.** The VFS works in any host SQLite. diff/promote/undo need the session API, which the extension interface does not provide, so they are available only when the host exports it; the extension checks at load time and the SQL functions then fail with a message that names the missing piece. (Decided: a second SQLite in the application's process is out of scope.)
- **Which hosts expose the session API (survey, step 1).** A loadable extension reaches the host's SQLite only through `sqlite3ext.h`, which has no session functions, so it has to find `sqlite3session_*` in the process itself. Measured on this machine, 2026-10-13, unless the last column says otherwise:

  | Host | The host's SQLite has the session API | An extension can call it | How I know |
  |---|---|---|---|
  | Ubuntu system libsqlite3 3.45.1 | yes (`ENABLE_SESSION`, `ENABLE_PREUPDATE_HOOK`) | **yes**: the shared library exports `sqlite3session_create` and `sqlite3changeset_apply` | measured (`nm -D`, an extension linked to it ran the API in Python) |
  | Python 3.12.3 (system build, uses that library) | yes | **yes**, same library | measured (`PRAGMA compile_options`) |
  | Node 24.19.0, built-in `node:sqlite` (SQLite 3.53.3) | yes, and `DatabaseSync.createSession` exists | **no**: the node binary exports no `sqlite3_*` symbols, and an extension loaded with `loadExtension` found no session symbol | measured (`nm -D` on the binary; an extension that tested for the symbol returned 0) |
  | better-sqlite3 | not mentioned in its compilation notes | unknown | documentation only, not tested |
  | Go, mattn/go-sqlite3 | its README lists a preupdate-hook build tag and no session tag | unknown | documentation only, not tested |
  | Rust, rusqlite | has a `session` cargo feature (needs `SQLITE_ENABLE_SESSION` in the SQLite it builds) | unknown | documentation only, not tested |
  | macOS system SQLite, python.org builds, Windows builds | not checked | not checked | not checked |

  Reading it: the extension can drive diff, promote and undo itself only where the host uses a shared system SQLite (Linux distributions and what is built on them, Python on them). **Where the host's SQLite is built into the runtime (Node), the host's own session API is available to the application but not to our extension**, so diff, promote and undo would have to be driven from the host language, with our code supplying the branch operations and the schema checks. That affects how step 3 is cut (see the open question below); it does not affect the VFS, which works in every host.
- **Open question for step 3, not decided:** build the branch layer (`fkx`) as a C library that takes SQLite as given and is used both by the CLI (own SQLite) and by an extension on shared-library hosts, with the changeset exchanged as bytes so that a host like Node can supply its own session calls. This keeps the "no second SQLite" decision and gives every host the operations, at the price of a host-side binding per language. To be designed with step 3.
- **CLI: its own SQLite, built with the two flags (`vfs/build.sh` already builds variants).** It is a separate program, so it adds no second SQLite to any application. The store is single-process, so the CLI can only work on a store no application has open: offline inspection, diff, promote, undo and scripted use, not a live sandbox an agent is writing to. That limit goes in the CLI's documentation.
- **Rejected (decided): a SQL-level diff and apply of our own**, and **a bundled SQLite inside the library**. The first would reimplement conflict detection, inversion and value encoding, the part the experiments found to be subtle (triggers, foreign keys, positional columns), and carry its own test burden; the second puts two SQLites in one process (symbol clashes, version skew). Reconsider the first only if the host survey shows common hosts lack the session API.

## Cost of the session build

5 repetitions each, medians, in [vfs/results/session-hook-overhead.txt](../vfs/results/session-hook-overhead.txt). One machine (WSL2), so read the ratios, not the milliseconds.

| | SQLite as built now | session options compiled in, no session attached | session attached to every table |
|---|---|---|---|
| 1,000,000 inserts and an update of a third of them, one transaction, in memory | 999 ms (967 to 1031) | 1,007 ms (971 to 1034) | 1,330 ms (1300 to 1418) |
| 3,000 single-row commits through the VFS | 11,698 ms | 11,581 ms | 11,783 ms |

- **Compiling the options in costs nothing measurable here** (the in-memory runs of the two builds overlap). That is the cost every user of our SQLite would pay, since the session is only attached by diff and promote.
- **Recording while attached is a real cost on a CPU-bound load** (about 32 percent on the bulk insert), which is why diff compares after the fact and does not record all the time.
- **The commit loop shows no difference**: it is dominated by the sync, about 3.9 ms a commit on this machine. A real-disk figure was not taken.

## Build plan

Each step is its own PR, reviewed before the next.

1. **Session build, the host survey and the VFS contract. Done.** `vfs/build.sh` has the `session` variant and extracts the header; the VFS gate runs on it (T15); the super-journal is refused, with a reason logged and a counter, and with journal modes MEMORY and OFF refused so that the refusal cannot be bypassed (T14, T6, the power-loss workload attempts a cross-branch write at every step and checks that nothing leaks; two new mutants); the survey above. The in-memory-main limit is recorded, not closed.
2. **Store format v4. Done** (see [DURABLE-STORE.md](DURABLE-STORE.md), State ids, metadata and reset): `RESET`, metadata (`FORKM` and `META` records), state ids, upgrade by compaction (a version 3 file refuses the new calls with `FK_EVERSION` until it is compacted). Differences from this note: a reset copies the source's state id instead of replacing it (equal ids have to mean equal content); there is no `fk_branch_parent`; the read-only flag is not enforced by the store (the layer enforces it, from the metadata). The power-loss harness's model got the new operations and a check that state ids are equal exactly when the content is; nine new mutants, each caught.
3. **The branch layer (`fkx`):** fork with base, diff, promote (fast-forward and replay with the policy), undo, garbage collection, retention. Tests include crash sequences across the several records of one operation, and every refusal in the policy table. **Design written: [FKX-DESIGN.md](FKX-DESIGN.md) and [fkx/fkx.h](../fkx/fkx.h); split into the core (3b) and the session driver (3c).**
4. **SQL functions and the CLI.**

## Not covered

One process, one thread (as the store). Two forks merged into each other (only fork into parent). Re-basing a fork. Replaying DDL (added tables and columns are the obvious next case; the data of a new table would need an empty base to diff against). Virtual tables and FTS shadow tables. Large changesets are held in memory (a streaming interface exists). The cost figures are one machine, WSL2, and in-memory or small-commit workloads; a real-disk figure for the diff was not taken.

## Decisions (Ben, 2026-10-12)

1. **History is snapshots** (no changeset blobs stored, two branch ids per undo point). Decided.
2. **Format v4** (reset, metadata, state id) is the step after the session build and before any layer code. Decided.
3. **Tables without a PRIMARY KEY are a documented limitation.** Read as: replay refuses a fork that changed rows of such a table and says which table; fast-forward is still allowed. It is never a silent drop. Decided; the wording "refuses and says why" is my reading of it.
4. **Packaging:** the library/extension needs a host that exposes the session API and says so when it does not; the CLI is built with its own SQLite for offline use. No bundled SQLite in the library and no SQL-level diff of our own. Decided.
5. **A transaction writes one branch**, enforced by the VFS refusing the super-journal (option A). Decided for v0.1; atomic multi-branch commit stays out unless a feature needs it.
