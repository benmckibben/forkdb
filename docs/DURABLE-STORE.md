# Durable layer store

Status: **implemented and tested in isolation** ([store/](../store), [test/](../test)); not yet connected to the SQLite VFS. This document says what the store promises, how it keeps the promise across a power cut, how that was tested, and what it does not do.

The store holds the pages of many database branches in **one append-only log file**. A fork is one small record. Nothing here depends on SQLite: it is a page store with branches, and the VFS from the spike will sit on top of it.

## What it promises

- **Atomic, durable commits.** A transaction (a set of page writes plus a new database size) becomes visible all at once. When `fk_txn_commit` returns `FK_OK`, it survives a power cut.
- **Nothing partial after a crash.** After a power cut, recovery yields exactly the state after the last acknowledged operation, or after that plus the one operation that was in flight. Never a mixture.
- **O(1) fork, discard.** `fk_fork` and `fk_discard` write one small record and sync once, whatever the data size.
- **Fail-stop on I/O errors.** After any failed write or fsync the store refuses everything (`FK_EFAILED`) until it is closed and reopened, and recovery decides what survived. It never retries a failed fsync (a retry can report success for data the kernel has already dropped).
- **Reads are checked.** Every page read verifies its record's checksum and page number.

## File format

```
[ 0 .. 4096)   file header: "FORKDBST", version, page size, crc;  two 32-byte marker slots at 512 and 1024
[ 4096 .. )    records, back to back
```

Record = 32-byte header + payload. Header: magic `FKR1`, type, **sequence number**, payload length, **crc of the previous record**, and a CRC-32C over the header (with its own crc field zeroed) and the payload.

| Type | Payload |
|---|---|
| PAGE | branch, page number, 4096 bytes |
| COMMIT | branch, number of PAGE records in this transaction, new size in pages |
| FORK | source branch, new branch id |
| DISCARD | branch |

A transaction is `PAGE* COMMIT`. A fork or discard is a single record. All of them end with **one fsync**.

## Recovery

On open: read the header, then scan records from offset 4096 and accept a record only if all of these hold: valid magic and length, the next sequence number, a previous-crc equal to the last accepted record's crc, and a correct checksum. The scan stops at the first record that fails any of them. PAGE records are held back and applied only when their COMMIT arrives (right branch, right page count, every page inside the new size); FORK and DISCARD apply at once. Whatever follows the last complete unit (a torn page, an uncommitted transaction, garbage) is **truncated away and the truncation fsynced**, so new records start on a clean boundary.

Why that is enough:
- A transaction whose COMMIT did not reach the disk is simply not applied, however many of its pages did. One whose pages are incomplete cannot reach its COMMIT, because the chain of sequence numbers and checksums breaks at the missing page.
- Pages and COMMIT are written with plain writes and **one** fsync at the end. The disk may persist them in any order, and tear any of them; recovery only trusts a prefix of the log whose records are all valid, and the commit is acknowledged only after the fsync, when everything is durable.
- Recovery is idempotent and crash-safe: its only writes are the truncation and its fsync, and doing it twice, or being cut off in the middle, leaves the same recoverable file.
- Opening a file that is empty, or whose creation was cut short (records are only written after the header is durable), makes a new store. A non-empty file that does not have our header is refused and never overwritten.

### Torn tail or damaged data? The durable-end marker

A log alone cannot tell **a torn tail** (normal after a power cut; cut it off) from **damage inside data that was once durable** (bit rot; cutting it off would destroy committed data). The first version of the store did exactly that, and a file test showed it: corrupting one committed record silently dropped everything after it.

The fix is a marker: the log length that was durably committed, written **after** the data it covers has been fsynced, into one of two alternating 32-byte slots (each in its own sector, each with a generation number and a checksum). It is refreshed every 32 operations and on a clean close. A power cut can only leave an older marker, never one that points past durable data. If recovery's valid prefix ends before the marker, the store reports `FK_ECORRUPT` and **does not truncate**, so the file can still be salvaged.

The limit of this: damage in the part of the log after the last marker is still indistinguishable from a torn tail and is cut off. At most 32 operations are exposed, and nothing after a clean close.

## In-memory index

Rebuilt by the same scan. Each branch has a head layer; a layer maps page number to the offset of its PAGE record (or a tombstone, for pages removed by shrinking). Forking freezes the source's head and gives both sides an empty one on top (O(1); an unchanged head is shared instead of stacked). A read walks the layer chain, then reads and checks one record. As in the spike, long chains need flattening; that is not implemented.

## How it was tested

The point of this work was to test the claim "nothing partial after a power cut" harder than the claim itself, so the harness is deliberately pessimistic. [test/simdisk.c](../test/simdisk.c) models a file with a volatile write cache:
- writes and truncations are **pending** until an fsync makes them durable;
- at a power cut the surviving image is the durable data plus **any subset** of the pending operations, applied in **any order**; a surviving write may be **torn** (any subset of its 512-byte sectors, or a byte prefix); the gap before a write that extends the file may hold zeros or garbage;
- an fsync can also fail, leaving everything pending.

[test/crash_test.c](../test/crash_test.c) runs seeded random workloads (commits of 1-3 pages that can grow or shrink the database, forks, discards, up to 8 branches) against an oracle model. For **every I/O event** of every workload it cuts the power just before the event, builds the post-crash images (all subsets when at most 8 operations are pending, a random sample of 130 otherwise, each both clean and torn), and checks that:
1. the store **opens**;
2. the recovered state equals the oracle after exactly the acknowledged operations, or those plus the one in flight (every branch, every page, every size);
3. the store is **usable afterwards**: the rest of the workload is replayed on it, then it is closed, reopened and compared with the oracle's final state;
4. **a power cut during recovery** itself (at each of its I/O events, with the same variety of images) is also survivable;
5. after a **failed fsync** the store is fail-stop, and the same checks hold for the disk it leaves behind.

Results are recorded in the test output; the numbers for the last full run are in the pull request that added this.

### Do the tests catch bugs? (mutants)

A test that has never failed proves little, so the store has compile-time bugs (`-DFK_BUG_*`) and `scripts/test-store.sh` checks that at least one test fails for each:

| Mutant | Power-loss harness | File tests |
|---|---|---|
| commits acknowledged without fsync | **catches** (a committed transaction is lost) | no |
| durable-end marker written before the data it covers is synced | **catches** (recovery wrongly reports corruption) | no |
| recovery does not truncate the torn tail | no | **catches** (the tail stays in the file) |
| recovery does not check the record chain | no | **catches** (a record from another history is accepted) |
| both of the previous two | no | **catches** |

Two honest notes. First, I expected the power-loss harness to catch the last three as well (stale records resurfacing after a torn tail); it did not, including a run of about 330 workloads of 12 operations (about 273,000 crash images) with the last row's bugs, which I stopped at a time limit. A stale record has to line up exactly with the new tail, which random workloads almost never produce. The two mechanisms also protect the same property, so either alone is enough in practice. The deterministic file tests are what show each one is needed. Second, "no" for the harness is a statement about this harness, not about the bugs.

## What this does not cover

- **The disk model is an assumption.** It assumes 512-byte sector atomicity, that fsync really makes data durable, and that data does not rot while the machine runs. A drive that lies about fsync, or a file system that loses a file's size update, is outside it. The directory entry of a newly created file is fsynced on the real backend but not modeled.
- **One process, one writer transaction at a time**; no locking between processes.
- **No garbage collection, compaction or layer flattening.** The log only grows; discarded branches and overwritten pages stay in it. This is the next real problem.
- **A tombstone-heavy shrink** is tested for correctness but not for cost.
- **Limits are small:** 1024 branches, 32-bit page numbers, whole transactions held in memory until commit.
- **It is not yet the SQLite VFS.** Connecting them (journal and WAL files, the checkpoint-then-fork rule, locking) is separate work.
- Tests ran on one machine, on a real file only for a normal close, a process killed without closing, an appended torn tail and a damaged record. A real power cut was not performed.

## Running the tests

```sh
scripts/test-store.sh          # file tests, power-loss tests under sanitizers, mutant checks
scripts/test-store.sh quick    # fewer workloads
SEEDS=300 OPS=10 scripts/test-store.sh
```

Needs only gcc.
