# fkx: the branch layer. Design note

Status: **design only; no implementation.** The proposed API is [fkx/fkx.h](../fkx/fkx.h) (it compiles as C with @-Wall -Wextra@; it was not checked as C++). This is step 3 of the plan in [SESSION-DESIGN.md](SESSION-DESIGN.md). Nothing below was measured except where it says so: it is design, reasoned from the store, the VFS and the experiments already in the repository.

fkx is a C library over the store and the VFS. It gives branches names, remembers what a fork was forked from, and implements the v0.1 operations: **init, fork, snapshot, diff, reset, discard, promote**, plus undo. The CLI and the SQL functions (step 4) are thin layers over it.

## What it is made of

| Piece | Needs | Provides |
|---|---|---|
| **fkx core** | the store and the VFS; no session API | open and close a repository, `fork`, `snapshot`, `discard`, `reset`, `list`, `info`, `open_db`, garbage collection, and the repository metadata |
| **stepwise primitives** | plain SQL on SQLite connections | `fkx_diff_open` and `fkx_replay_begin`: the schema policy, the table classification, and the replay transaction, without any session call |
| **session driver** (`fkx_session.c`) | a SQLite built with the session extension | `fkx_diff`, `fkx_promote` (replay path), `fkx_undo` (replay path), built on the primitives; without it these return `FKX_ENOSESSION` |

The split follows the host survey in SESSION-DESIGN.md: where the host's SQLite has the session API but an extension cannot reach it (Node's built-in SQLite), the host can still call its own session functions and use the primitives, exchanging changesets as bytes. That is a host-side binding per language, not designed here; the primitives are the seam.

## Identity, names and metadata

Store branch ids are reused (see DURABLE-STORE.md, Branch ids), so nothing persistent may refer to a branch by id. Every branch fkx makes gets a **uid**, 64 random bits from the operating system, never reused (a collision among at most 1,024 live branches is about 2^-45; fkx checks the index and draws again). Persistent references are uids. They are resolved through an in-memory index built by one pass over the branches' metadata at open (at most 1,024 small blobs). A **ref** in the API is a user name, or `#` and 16 hex digits for a uid; that is how internal branches are addressed. Names are 1 to 63 bytes of letters, digits and `. _ - /`, not starting with `#`, and unique among the live named branches (root, forks, snapshots).

Each branch carries one metadata blob (the store's `fk_meta_set`, at most 512 bytes), written in the same record as the branch's creation (`fk_fork_meta`), so a branch never exists without its label. Layout, little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `FKX1` |
| 4 | 1 | kind: 1 root, 2 fork, 3 snapshot, 4 base, 5 undo |
| 5 | 1 | flags: bit 0 read-only (set on snapshot, base, undo) |
| 6 | 1 | name length (0 for base and undo) |
| 7 | 1 | number of undo entries (root and forks) |
| 8 | 8 | uid |
| 16 | 8 | origin uid (fork, snapshot: where it was forked or taken from) |
| 24 | 8 | base uid (fork) |
| 32 | 8 | owner uid (base, undo) |
| 40 | name length | name |
| then | 16 each | undo entries, newest first: uid of the *before* snapshot, uid of the *after* snapshot |

The fixed part is 40 bytes, so a 63-byte name and 16 undo entries use 359 of the 512 (this is why `undo_keep` is capped at 16). A format change inside this blob is a new magic or a version byte, and old readers refuse what they do not know.

**Branch 0** is the root: kind 1, named `main` unless the repository was created with another name. A store whose branch 0 has no fkx metadata and which has no other live branch is adopted at open (the root metadata is written); any other store with branches fkx did not make is refused (`FKX_ECORRUPT`). Mixing raw forks into a managed repository is not supported in v0.1.

## Operations, and what each one writes

Everything is built from the store's records, each atomic and durable on its own. An operation that needs more than one is **not atomic**, so each is ordered so that a power cut leaves garbage that can be recognised and removed, never a half-changed named branch.

| Operation | Records, in order | A cut between them leaves |
|---|---|---|
| **fork** F from P | 1. FORKM: base A = snapshot of P (kind base, owner F's uid, read-only). 2. FORKM: F = fork of A (kind fork, origin P, base A) | After 1: an orphan base (its owner does not exist). Removed by gc. |
| **snapshot** S of P | 1. FORKM: S = fork of P (kind snapshot, read-only) | nothing: one record |
| **discard** F | 1. DISCARD F. 2. DISCARD its base. 3. DISCARD each undo point of F | Bases and undo points whose owner is gone. Removed by gc. The owner goes first, so a live branch never points at a dead one. |
| **reset** F to X | 1. RESET F := X (default X = the base of F) | nothing: one record. F's metadata is kept |
| **promote** F into P, fast-forward | 1. FORKM: *before* = snapshot of P (kind undo). 2. RESET P := F. 3. FORKM: *after* = snapshot of P. 4. META: P's metadata with the new undo entry (oldest dropped if over retention) | After 1 or 3: an undo snapshot P does not list. After 2: P is promoted and **this one promote cannot be undone**. Never a half-promoted P. |
| **promote** F into P, replay | 1. FORKM: *before*. 2. **one SQLite transaction on P** (one store commit). 3. *after*. 4. META | as above |
| **undo** on P | exact: 1. RESET P := before. 2. META with the entry removed (and the two snapshots discarded after). Replay: one SQLite transaction on P, then the same META | after 1, P is undone and the entry is still listed: a second undo would find that P no longer matches `after` and use the replay path |

**Garbage collection** (`fkx_gc`, also run at open): discard (1) a base or an undo point whose owner uid is not alive, (2) an undo point its owner does not list, (3) a base its owner does not name as its base, (4) undo points beyond `undo_keep`. A named branch (root, fork, snapshot) is never collected. With nothing running concurrently (it is one thread) a listing that is incomplete can only mean an interrupted operation.

**The id budget.** A store has FK_MAX_BRANCHES (1,024) live branch ids. A fork costs two, a snapshot one, an undo point two (so 2 x `undo_keep` per branch that was promoted into: 16 at the default of 8). `FKX_ELIMIT` says so with the numbers. Lowering `undo_keep` takes effect at the next gc.

## Promote, in detail

**Fast-forward** is allowed when `state(P) == state(base of F)` (the store's state ids, step 2) **and no connection is open on P**; the second condition is the reset hazard recorded in DURABLE-STORE.md (SQLite decides whether its page cache is stale from a counter that can repeat after a reset; reasoned, not tested). It takes the *before* snapshot, resets, takes the *after* snapshot. Exact, O(1), carries DDL.

**Replay** is the path when P has moved or a connection is open on it. It is only done if the policy allows: it begins with `fkx_replay_begin`, which opens a connection on P (with `foreign_keys=ON` set before the transaction), `BEGIN IMMEDIATE`, checks the schema policy of SESSION-DESIGN.md (the fork changed no DDL; the touched tables have the same `PRAGMA table_xinfo` in P as in the base; the fork changed no row of a table without a primary key, compared both ways with `EXCEPT`; no virtual tables), drops the triggers on the touched tables, and hands the connection to the driver, which applies the changeset with the conflict handler answering ABORT and NOSAVEPOINT. Commit re-creates the triggers and commits: **P changes at that commit, which is one store commit.** Abort, a refusal, a conflict or a crash leave P as it was. These steps are the ones the experiments in SESSION-DESIGN.md tried (X4 to X6).

If both fast-forward and replay are impossible (a connection open on P and the fork changed DDL), `fkx_promote` returns `FKX_EBUSY` and says which connections to close, and no promote half-happens. If P already holds what F holds (`state(P) == state(F)`) the result is `FKX_METHOD_UP_TO_DATE` and nothing is written. A promoted fork keeps its old base, so promoting it a second time replays its work onto a P that already has it and conflicts: the intended use is to discard the fork or fork again. Re-basing a fork is not designed.

**Dry run** performs the same steps inside a transaction that is rolled back, with the conflict handler answering OMIT, so every conflict is listed and not only the first.

## Undo

An undo point is two read-only internal snapshots of the target, *before* and *after* a promote, listed in the target's metadata. `fkx_undo` on P takes the newest entry. If `state(P) == state(after)` and nothing is open on P, **RESET P := before** (exact). Otherwise it replays `diff(after, before)` onto P by the replay path (the work done on P since is kept; a conflict aborts and changes nothing). The entry and its two snapshots are removed afterwards. Nothing is written into the user's database.

## Read-only branches, and what the VFS must add

The VFS has to know which branches may be written and which have connections open. Both are small additions, in the implementation PR for the core (3b):

- `fkvfs_branch_refs(v, id)`: the number of connections open on a branch (the VFS already counts them; the number is only not exposed). fkx uses it for the `FKX_EBUSY` cases and for `open_connections`.
- `fkvfs_set_guard(v, fn, ctx)`: a callback the VFS calls in `xOpen` with the branch id and whether the open is for writing; it returns whether to allow it. fkx installs one that refuses writes to a branch whose metadata says read-only, so a base cannot be corrupted by an application that opens `b7` directly through the VFS instead of through `fkx_open_db`. The refusal is `SQLITE_READONLY` (reasoned: opening read-write a file the VFS refuses falls back to read-only in SQLite only when the caller passes the flag for it; to be tested).

## Concurrency and the application's connections

One process and one thread, like the store and the VFS. Connections are ordinary SQLite connections made by `fkx_open_db` (or by the application directly, through the VFS, with names like `b7`; the latter works but bypasses name resolution).

**An application that keeps a connection open on its main branch cannot be fast-forwarded into** (and cannot have a branch reset or discarded under it): it has to close the connection around the operation, or let promote take the replay path when the policy allows. This follows from the cache hazard above and is the largest practical cost of the design. The offline CLI is not affected (nothing is open). There is a way to remove it, **not designed in detail and not tested**: SQLite decides its page cache is stale by reading 16 bytes at offset 24 of the file at the start of every transaction; a VFS that returns a per-handle altered value for those 16 bytes after a reset would make every open connection drop its cache, at the price of one spurious cache flush after each of that connection's own writes. If keeping connections open turns out to matter, that is the thing to try.

## What was not designed

- **Re-basing** a fork onto its origin, and merging two forks into each other (only fork into origin).
- **Replaying DDL** (a new table, a new column). The refusal says what differs. Fast-forward carries any change.
- **A bulk or streaming interface for large changesets** (they are held in memory).
- **Auto-compaction and an open-time index checkpoint** (a separate item: opening costs about 0.4 s per GiB of log).
- **Several repositories in one process** with the same VFS name (each registers its own).

## Testing plan

The implementation is tested the way the store and the VFS are: the **power-loss harness** runs the fkx operations on the simulated disk and checks, after every cut, that every named branch is either as it was or as the operation leaves it (never half-promoted), that the index is consistent (unique names and uids, every fork's base alive, every undo entry's snapshots alive), that `fkx_gc` removes exactly the garbage the cut can leave and nothing else, and that a replayed promote equals `diff` applied by hand. **File tests** cover the errors (`FKX_EBUSY` with a connection open, `FKX_EREADONLY`, `FKX_ELIMIT`, names), adoption and refusal of a foreign store, and an upgrade from a version 3 file. **Mutants**, each of which must be caught: a promote that resets before taking the undo snapshot, a fork that writes the fork before the base, a discard that removes the base first, a gc that collects a listed undo point, a fast-forward decided without the state id, a replay that does not drop triggers, one that does not force foreign keys on, an undo that does not check the state id, a stale name index after a discard. The session driver is tested with the cases of experiments X1 to X6 as assertions.

## Build plan

1. **This PR:** the note and the header. No code.
2. **3b, the core:** the metadata codec, the repository (open, close, gc, the index), fork, snapshot, discard, reset, list, info, open_db, fast-forward promote and undo by reset; the two VFS additions; tests and mutants as above for those. Needs no session API.
3. **3c, the session driver:** the stepwise primitives, @fkx_diff@, replay promote, undo by replay; the schema policy with a test for each refusal; the rest of the mutants. Runs on the @session@ build the VFS gate already has.
4. Step 4 of SESSION-DESIGN.md (SQL functions and the CLI) follows.

## Decisions needed

1. **Fast-forward and reset need no open connection on the target** (v0.1), with the cache-invalidation trick left as a later option. Or should I try the trick first, before 3b? (I recommend not: it is a VFS change for a case the offline CLI does not have, and replay covers DML-only forks.)
2. **User snapshots are a first-class kind** (the v0.1 operation list has *snapshot*), named, read-only, usable as a `reset` target and a `diff` operand. Fine?
3. **uids are random 64-bit numbers** kept in each branch's metadata, with no persisted counter (a counter would need a write for every fork). Fine?
4. **A store with branches fkx did not make is refused**, not adopted. Fine for v0.1?
5. **The PR split above** (3b core, 3c session driver), each with its own tests and mutants.
