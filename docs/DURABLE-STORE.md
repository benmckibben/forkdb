# Durable layer store

Status: **implemented and tested in isolation** ([store/](../store), [test/](../test)), including compaction; not yet connected to the SQLite VFS. This document says what the store promises, how it keeps the promise across a power cut, how that was tested, and what it does not do.

The store holds the pages of many database branches in **one append-only log file**. A fork is one small record. The log is rewritten by compaction when it fills with pages nobody can see any more. Nothing here depends on SQLite: it is a page store with branches, and the VFS from the spike will sit on top of it.

## What it promises

- **Atomic, durable commits.** A transaction (a set of page writes plus a new database size) becomes visible all at once. When `fk_txn_commit` returns `FK_OK`, it survives a power cut.
- **Nothing partial after a crash.** After a power cut, recovery yields exactly the state after the last acknowledged operation, or after that plus the one operation that was in flight. Never a mixture.
- **O(1) fork, discard.** `fk_fork` and `fk_discard` write one small record and sync once, whatever the data size.
- **Fail-stop on I/O errors.** After any failed write or fsync the store refuses everything (`FK_EFAILED`) until it is closed and reopened, and recovery decides what survived. It never retries a failed fsync (a retry can report success for data the kernel has already dropped).
- **Reads are checked.** Every page read verifies its record's checksum and page number.
- **Bounded size.** `fk_compact` rewrites the file so that it holds only the pages some live branch can still see, crash-safely, and never copies a damaged page.

## File format

```
[ 0 .. 4096)   file header: "FORKDBST", version (2), page size, crc;  two 32-byte marker slots at 512 and 1024
[ 4096 .. )    records, back to back
```

Record = 32-byte header + payload. Header: magic `FKR1`, type, **sequence number**, payload length, **crc of the previous record**, and a CRC-32C over the header (with its own crc field zeroed) and the payload.

**Checksum implementation.** CRC-32C is computed with the CPU's CRC32 instruction where there is one (x86-64 with SSE4.2, checked at run time, so one binary runs everywhere; AArch64 when built with the CRC extension) and with a portable table loop otherwise. It is the same function, so the format does not depend on which one a machine has: the tests check both against the standard test vectors and a bit-by-bit reference on 20,000 random buffers at every alignment, and write a log with each and read it with the other (the two files are byte-for-byte identical). On the benchmark machine the hardware version made compaction of a 2 GiB database 2.7 times faster and opening a 4 GiB log 2.2 times faster (see the benchmark PR for the method and caveats).

| Type | Payload |
|---|---|
| PAGE | branch, page number, 4096 bytes |
| COMMIT | branch, number of PAGE records in this transaction, new size in pages |
| FORK | source branch, new branch id |
| DISCARD | branch |
| CPAGE | a page copied by compaction: same layout as PAGE, branch field unused |
| SNAP | one chunk (up to 4 KiB) of the snapshot byte stream |
| SNAPEND | length and checksum of the snapshot byte stream |

A transaction is `PAGE* COMMIT`. A fork or discard is a single record. All of them end with **one fsync**. A compacted file starts with one `CPAGE* SNAP* SNAPEND` unit (see Compaction). Version 1 files (before compaction existed) still open; a snapshot inside one is refused.

## Recovery

On open: read the header, then scan records from offset 4096 and accept a record only if all of these hold: valid magic and length, the next sequence number, a previous-crc equal to the last accepted record's crc, and a correct checksum. The scan stops at the first record that fails any of them. PAGE records are held back and applied only when their COMMIT arrives (right branch, right page count, every page inside the new size); FORK and DISCARD apply at once. Whatever follows the last complete unit (a torn page, an uncommitted transaction, garbage) is **truncated away and the truncation fsynced**, so new records start on a clean boundary.

Why that is enough:
- A transaction whose COMMIT did not reach the disk is simply not applied, however many of its pages did. One whose pages are incomplete cannot reach its COMMIT, because the chain of sequence numbers and checksums breaks at the missing page.
- Pages and COMMIT are written with plain writes and **one** fsync at the end. The disk may persist them in any order, and tear any of them; recovery only trusts a prefix of the log whose records are all valid, and the commit is acknowledged only after the fsync, when everything is durable.
- Recovery is idempotent and crash-safe: its only writes are the truncation and its fsync, and doing it twice, or being cut off in the middle, leaves the same recoverable file.
- Recovery also deletes a leftover `<file>.compact` from an interrupted compaction; it is never read.
- Opening a file that is empty, or whose creation was cut short (records are only written after the header is durable), makes a new store. A non-empty file that does not have our header is refused and never overwritten.

### Torn tail or damaged data? The durable-end marker

A log alone cannot tell **a torn tail** (normal after a power cut; cut it off) from **damage inside data that was once durable** (bit rot; cutting it off would destroy committed data). The first version of the store did exactly that, and a file test showed it: corrupting one committed record silently dropped everything after it.

The fix is a marker: the log length that was durably committed, written **after** the data it covers has been fsynced, into one of two alternating 32-byte slots (each in its own sector, each with a generation number and a checksum). It is refreshed every 32 operations and on a clean close. A power cut can only leave an older marker, never one that points past durable data. If recovery's valid prefix ends before the marker, the store reports `FK_ECORRUPT` and **does not truncate**, so the file can still be salvaged.

The limit of this: damage in the part of the log after the last marker is still indistinguishable from a torn tail and is cut off. At most 32 operations are exposed, and nothing after a clean close.

## In-memory index

Rebuilt by the same scan. Each branch has a head layer; a layer maps page number to the offset of its PAGE record (or a tombstone, for pages removed by shrinking). A layer is an **open-addressing hash table** (linear probing, never more than half full, keyed by page number), so its memory is proportional to the number of pages it holds, not to the highest page number it touched: a fork that writes one page near the end of a 1 GiB database costs about 100 bytes instead of 2 MiB. Entries cost 12 bytes in a table that is a quarter to half full, so 24 to 48 bytes per page held; a layer that holds every page of a database costs about three times what a dense array would (6 MiB instead of 2 MiB per GiB), which I accepted for a layer holding all pages of a database. A shrink adds a tombstone only for pages that exist below the layer. Compaction sorts a layer's entries by page to plan the new file; the snapshot format already listed pages sparsely, so it did not change. Forking freezes the source's head and gives both sides an empty one on top (O(1); an unchanged head is shared instead of stacked). A read walks the layer chain, then reads and checks one record. Chains only get shorter at compaction (below); between compactions a long fork history makes reads walk more layers.

## Compaction

**What is garbage.** A page record is live only if it is the visible version of some page in some live branch: walk each live branch's layer chain, newer layers hiding older ones, shared layers counted once. Discarded branches, overwritten pages, pages hidden by a shrink and the uncommitted tail fall out of that. Layers get flattened too: a frozen layer that has exactly one child and no branch of its own is folded into the child, which is what remains after one side of a fork is discarded. Tombstones are only kept where there is a layer below for them to hide.

**Procedure** (`fk_compact`, or `fk_compact_if_needed(min_bytes)`, which compacts when the file is at least that big and at least half of it is garbage):
1. Plan from the in-memory index. Nothing is written yet.
2. Create `<file>.compact`. Write a header, then every live page once as a CPAGE record, **verifying each against its checksum as it is read from the old file**. A damaged page aborts the compaction: it never launders damage into a fresh, valid-looking file.
3. Write the layer graph (layers, parents, page-to-offset maps, branch heads) as a byte stream in SNAP records, then SNAPEND with its length and checksum. Sharing between forks survives, so a fork does not turn into a copy.
4. Write the durable-end marker, then **one fsync**. Until the rename nothing refers to this file, so one fsync covers all of it (the marker's ordering rule matters only for a file that is visible).
5. Rename it over the store's file, then fsync the directory.
6. Load the snapshot we just wrote into memory with the same loader recovery uses, so the running store and a recovered store are the same thing.

**Crash safety.** The only commit point is the rename.
- Before it, the old file is untouched. A leftover temporary file is deleted at the next open.
- After it, the new file is complete (it was synced before the rename).
- The directory fsync makes the rename durable. Until it returns, a power cut may bring back either file, and both hold the same data; the compaction is not acknowledged before then, and nothing is written to the new file before then.
- Recovery treats a snapshot as "replace the index with this". A snapshot is only accepted as the first unit of a version 2 file, every offset in it must point at a CPAGE of that unit for the right page, and its checksum must match. The new file's record chain starts again at sequence 1.

**Errors.** Before the rename, any error (cannot create the temporary file, a write fails because the disk is full, an fsync fails) deletes the temporary file and returns the error; the store's file is untouched and the store stays usable. After the rename, an error is fail-stop like everywhere else: close and reopen.

**Cost and limits.** It rewrites all live data, so run it when there is a lot of garbage (hence the threshold), not on a schedule. The writer is blocked for the whole time; there is no online compaction. Peak disk use is the old file plus the live data; the store does not check free space first, it simply fails cleanly when the disk fills. I have not measured how long it takes on a large database and will not quote numbers before I have.

## How it was tested

The point of this work was to test the claim "nothing partial after a power cut" harder than the claim itself, so the harness is deliberately pessimistic. [test/simdisk.c](../test/simdisk.c) models a file with a volatile write cache:
- writes and truncations are **pending** until an fsync makes them durable;
- at a power cut the surviving image is the durable data plus **any subset** of the pending operations, applied in **any order**; a surviving write may be **torn** (any subset of its 512-byte sectors, or a byte prefix); the gap before a write that extends the file may hold zeros or garbage;
- an fsync can also fail, leaving everything pending;
- there are **two files**, the store's and the temporary one compaction writes, and **directory state**: a rename is visible at once but durable only after the directory sync. At a power cut a pending rename may or may not have happened, and the surviving file is resolved like any other (durable data plus any subset of its pending writes). The simulator allows one outstanding rename.

[test/crash_test.c](../test/crash_test.c) runs seeded random workloads (commits of 1-3 pages that can grow or shrink the database, forks, discards, **compactions**, up to 8 branches) against an oracle model. For **every I/O event** of every workload it cuts the power just before the event, builds the post-crash images (all subsets when at most 8 operations are pending, a random sample of 130 otherwise, each both clean and torn), and checks that:
1. the store **opens**;
2. the recovered state equals the oracle after exactly the acknowledged operations, or those plus the one in flight (every branch, every page, every size);
3. the store is **usable afterwards**: the rest of the workload is replayed on it, then it is closed, reopened and compared with the oracle's final state;
4. **a power cut during recovery** itself (at each of its I/O events, with the same variety of images) is also survivable;
5. after a **failed fsync** the store is fail-stop, and the same checks hold for the disk it leaves behind. The one exception is a compaction that fails before the rename (its fsync is on the temporary file): the store must stay usable, and it is checked to;
6. after every compaction, the **in-memory state still equals the oracle**, the file holds exactly the live data, and compacting again changes nothing.

Cutting the power before every I/O event of a compaction (creating the file, each page, each snapshot chunk, the marker, the fsync, the rename, the directory sync) is part of 1-4: the oracle does not change across a compaction, so the recovered state must be the same either way, whichever file survived.

The power cuts are expensive, so only about one workload in a few contains a compaction. A second, cheap pass therefore runs thousands of **longer workloads with several compactions each and no power cut**, checking the oracle after every operation and again after a close and reopen. It is what exercises layer folding and tombstone handling at volume.

Results are recorded in the test output; the numbers for the last full run are in the pull request that added this.

### Do the tests catch bugs? (mutants)

A test that has never failed proves little, so the store has compile-time bugs (`-DFK_BUG_*`) and `scripts/test-store.sh` checks that at least one test fails for each:

| Mutant | Power cuts | Logic pass (no cuts) | File tests |
|---|---|---|---|
| commits acknowledged without fsync | **catches** | no | no |
| durable-end marker written before the data it covers is synced | **catches** | no | no |
| recovery does not truncate the torn tail | no | no | **catches** |
| recovery does not check the record chain | no | no | **catches** |
| both of the previous two | no | no | **catches** |
| compaction: new file swapped in without being synced | **catches** (cut between the rename and the directory sync) | no | no |
| compaction: directory not synced after the rename | **catches** (commits acknowledged after the compaction are lost) | no | no |
| compaction: pages of a folded layer are lost | no | **catches** | **catches** |
| compaction: tombstones dropped where they hide a lower layer | no | **catches** | **catches** |
| compaction: copies a damaged page without checking | no | no | **catches** |
| open: a leftover compaction file is not deleted | no | no | **catches** |
| index: a rehash loses one entry (off-by-one in the loop) | no | no | **catches** (needs thousands of pages: the harness's 6 never fill a table enough) |
| index: a collision is treated as a miss | **catches** | **catches** | **catches** |
| index: overwriting a page adds a second entry instead | **catches** | **catches** | **catches** |
| hardware CRC32C leaves out the last bytes of a buffer | no | no | **catches** (the CRC test; the store still works with itself, so nothing else notices) |

Which test catches which is not what I expected in two places, and I would rather say so than let the table suggest more than it shows:
- I expected the power-loss harness to catch the torn-tail and record-chain mutants too (stale records resurfacing after a torn tail); it did not, including a run of about 330 workloads of 12 operations (about 273,000 crash images). A stale record has to line up exactly with the new tail, which random workloads almost never produce. The two mechanisms also protect the same property, so either alone is enough in practice. The deterministic file tests are what show each one is needed.
- The two layer-folding mutants **survived the power cuts**: 15 workloads of 10 operations with 15 compactions in them, 0 violations. The cuts are expensive, so few workloads reach the particular shape (discard one side of a fork, then compact) that folding needs. That is why the logic pass exists: it found both within a handful of workloads, and a deterministic file test now covers each as well.
- "no" is a statement about this harness, not about the bugs.

## What this does not cover

- **The disk model is an assumption.** It assumes 512-byte sector atomicity, that fsync really makes data durable, and that data does not rot while the machine runs. A drive that lies about fsync, or a file system that loses a file's size update, is outside it. The directory entry of a newly created file is fsynced on the real backend but not modeled.
- **One process, one writer transaction at a time**; no locking between processes.
- **Compaction blocks the writer** and rewrites all live data; it is not incremental and not online. A segmented log that reclaims cold segments one at a time would avoid both, at the price of a manifest for atomicity (roughly doubling the work); I would only move to it if whole-file rewrites turn out to hurt on real databases. I have not measured compaction time on a large database.
- **Nothing calls compaction automatically.** `fk_compact_if_needed` is there for the layer above to call (after a discard, say); the store itself never decides to.
- **Between compactions the log grows**, and reads walk the layer chain; compaction is what flattens it.
- **The simulator models one outstanding rename**, and a rename that is atomic (as POSIX requires) but is lost or kept as a whole at a power cut. A file system that can leave a zero-length file after a rename without a prior fsync is covered (the mutant that skips the fsync shows it); one that breaks rename atomicity is not.
- **A tombstone-heavy shrink** is tested for correctness but not for cost.
- **Limits are small:** 1024 branches, 32-bit page numbers, whole transactions held in memory until commit.
- **It is not yet the SQLite VFS.** Connecting them (journal and WAL files, the checkpoint-then-fork rule, locking) is separate work.
- Tests ran on one machine, on a real file only for a normal close, a process killed without closing (also in the middle of a compaction, before and after the rename), an appended torn tail, a spliced record, a damaged record, a damaged page met during compaction, a full disk (writes to the temporary file failing), and relabelled format versions. A real power cut was not performed.

## Running the tests

```sh
scripts/test-store.sh          # file tests, logic pass and power-loss tests under sanitizers, mutant checks
scripts/test-store.sh quick    # fewer workloads
SEEDS=300 OPS=10 scripts/test-store.sh
```

Needs only gcc.
