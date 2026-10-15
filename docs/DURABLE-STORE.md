# Durable layer store

Status: **implemented and tested** ([store/](../store), [test/](../test)), including compaction. The SQLite VFS ([vfs/](../vfs)) and the branch layer ([fkx/](../fkx)) are built on it. This document says what the store promises, how it keeps the promise across a power cut, how that was tested, and what it does not do.

The store holds the pages of many database branches in **one append-only log file**. A fork is one small record. The log is rewritten by compaction when it fills with pages nobody can see any more. Nothing here depends on SQLite: it is a page store with branches, and the VFS in [vfs/](../vfs) sits on top of it.

## What it promises

- **Atomic, durable commits.** A transaction (a set of page writes plus a new database size) becomes visible all at once. When `fk_txn_commit` returns `FK_OK`, it survives a power cut.
- **Nothing partial after a crash.** After a power cut, recovery yields exactly the state after the last acknowledged operation, or after that plus the one operation that was in flight. Never a mixture.
- **O(1) fork, discard.** `fk_fork` and `fk_discard` write one small record and sync once, whatever the data size.
- **Fail-stop on I/O errors.** After any failed write or fsync the store refuses everything (`FK_EFAILED`) until it is closed and reopened, and recovery decides what survived. It never retries a failed fsync (a retry can report success for data the kernel has already dropped).
- **Reads are checked.** Every page read verifies its record's checksum and page number.
- **Bounded size.** `fk_compact` rewrites the file so that it holds only the pages some live branch can still see, crash-safely, and never copies a damaged page.

## File format

```
[ 0 .. 4096)   file header: "FORKDBST", version (4), page size, crc;  two 32-byte marker slots at 512 and 1024
[ 4096 .. )    records, back to back
```

Record = 32-byte header + payload. Header: magic `FKR1`, type, **sequence number**, payload length, **crc of the previous record**, and a CRC-32C over the header (with its own crc field zeroed) and the payload.

**Checksum implementation.** CRC-32C is computed with the CPU's CRC32 instruction where there is one (x86-64 with SSE4.2, checked at run time, so one binary runs everywhere; AArch64 when built with the CRC extension) and with a portable table loop otherwise. It is the same function, so the format does not depend on which one a machine has: the tests check both against the standard test vectors and a bit-by-bit reference on 20,000 random buffers at every alignment, and write a log with each and read it with the other (the two files are byte-for-byte identical). On the benchmark machine (one run each, WSL2, see the pull request for the method and caveats) the hardware version took compaction of a 2 GiB database from 12.9 s to 4.7 s, opening a 4 GiB log from 11.4 s to 5.0 s, and a cached 4 KiB read from 8.9 µs to 1.6 µs.

| Type | Payload |
|---|---|
| PAGE | branch, page number, 4096 bytes |
| COMMIT | branch, number of PAGE records in this transaction, new size in pages |
| FORK | source branch, new branch id (a never-used id, or from version 3 the id of a discarded branch) |
| DISCARD | branch |
| CPAGE | a page copied by compaction: same layout as PAGE, branch field unused |
| SNAP | one chunk (up to 4 KiB) of the snapshot byte stream |
| SNAPEND | length and checksum of the snapshot byte stream |
| FORKM | (version 4) like FORK, plus the new branch's metadata: source, new id, length, bytes |
| RESET | (version 4) destination branch, source branch |
| META | (version 4) branch, length, bytes: replaces the branch's metadata |

A transaction is `PAGE* COMMIT`. A fork or discard is a single record. All of them end with **one fsync**. A compacted file starts with one `CPAGE* SNAP* SNAPEND` unit (see Compaction). Version 1 files (before compaction existed) still open; a snapshot inside one is refused. Version 2 files (before ids could be reused) still open and keep their rules until they are compacted; Version 3 files (before metadata, reset and state ids) still open and work as before, and the calls that write the version 4 records return `FK_EVERSION` on them until `fk_compact` rewrites the file in the current format (see State ids, metadata and reset). A file with a version this program does not know is refused with `FK_ECORRUPT` and nothing in it is touched.

## Recovery

On open: read the header, then scan records from offset 4096 and accept a record only if all of these hold: valid magic and length, the next sequence number, a previous-crc equal to the last accepted record's crc, and a correct checksum. The scan stops at the first record that fails any of them. PAGE records are held back and applied only when their COMMIT arrives (right branch, right page count, every page inside the new size); FORK and DISCARD apply at once. Whatever follows the last complete unit (a torn page, an uncommitted transaction, garbage) is **truncated away and the truncation fsynced**, so new records start on a clean boundary.

Why that is enough:
- A transaction whose COMMIT did not reach the disk is simply not applied, however many of its pages did. One whose pages are incomplete cannot reach its COMMIT, because the chain of sequence numbers and checksums breaks at the missing page.
- Pages and COMMIT are written with plain writes and **one** fsync at the end. The disk may persist them in any order, and tear any of them; recovery only trusts a prefix of the log whose records are all valid, and the commit is acknowledged only after the fsync, when everything is durable.
- Recovery is idempotent and crash-safe: its only writes are the truncation and its fsync, and doing it twice, or being cut off in the middle, leaves the same recoverable file.
- Recovery also deletes a leftover `<file>.compact` from an interrupted compaction; it is never read.
- Opening a file that is empty, or whose creation was cut short (records are only written after the header is durable), makes a new store. A "cut short" file is one that is shorter than the header, or exactly header-sized with no valid header, and in which every byte is zero or the byte our header has at that place (a prefix of the header, or zeros where the rest of a sector did not land). Any other file that does not have our header is refused with `FK_ECORRUPT` and never overwritten. (This sentence used to say so while the code took EVERY file of up to 4096 bytes for a cut-short creation and replaced it with an empty store: a 100-byte notes file, or a one-page SQLite database, was destroyed by `forkdb_init`. Found by the recovery fuzzer's first run; the tests above only had 5000-byte junk.) The price: under the power-loss model a torn header write can leave garbage where a sector did not land, and a store whose very first creation was interrupted that way is refused instead of recreated. Nothing had been acknowledged, so nothing is lost, and the harness allows exactly that refusal (`half-created stores refused` in its summary); delete the file and create the store again.

### Torn tail or damaged data? The durable-end marker

A log alone cannot tell **a torn tail** (normal after a power cut; cut it off) from **damage inside data that was once durable** (bit rot; cutting it off would destroy committed data). The first version of the store did exactly that, and a file test showed it: corrupting one committed record silently dropped everything after it.

The fix is a marker: the log length that was durably committed, written **after** the data it covers has been fsynced, into one of two alternating 32-byte slots (each in its own sector, each with a generation number and a checksum). It is refreshed every 32 operations and on a clean close. A power cut can only leave an older marker, never one that points past durable data. If recovery's valid prefix ends before the marker, the store reports `FK_ECORRUPT` and **does not truncate**, so the file can still be salvaged.

The limit of this: damage in the part of the log after the last marker is still indistinguishable from a torn tail and is cut off. At most 32 operations are exposed, and nothing after a clean close.

## Handing a page over

`fk_txn_write` copies the caller's page into the transaction's pending list. A caller that stages a whole transaction itself (the SQLite VFS does) would hold every page twice until the commit, so `fk_txn_write_owned(s, pgno, page)` takes a `malloc`'d `FK_PAGE`-byte buffer without a copy. On `FK_OK` the store owns the buffer and frees it at commit or at `fk_txn_abort`; the caller must not touch it again. On any error (`FK_EINVAL`: no open transaction or a NULL page, `FK_ENOMEM`) the caller still owns it. Nothing else about a transaction changes: the format, the commit record and the ordering are the same, so this is not a format change. The file and crash tests send every other page through this call (under the address sanitizer, which would report a leak or a double free), and `test/file_test.c` checks the ownership rules directly.

## Preallocating the log

Appending to a file extends its size and allocates blocks, and the sync after it must then commit that metadata through the filesystem journal as well as flush the data. Overwriting blocks that are already written needs only the data flush. On real disks that is the difference between a commit costing about three times what a WAL commit costs and costing the same (see the gap to WAL in [VFS-SPIKE.md](VFS-SPIKE.md)).

`store/io_prealloc.c` is an opt-in layer over any `fk_io` that keeps the file written ahead of the end of the log: when a write would pass the end of what has been written, it first writes zeros up to the next multiple of `chunk` bytes and syncs, once per chunk. The store and the format do not change.

- **Recovery is unchanged.** The zeros past the end of the log are not a valid record, so recovery treats them as a torn tail and cuts them off, exactly as it does after a power cut. `fk_prealloc_close` cuts them off on a clean close, so a file at rest is exactly the log.
- **The header is never preallocated.** Creating a store is `truncate(0)`, write the header, sync, and recovery takes a file whose header never landed for a creation that was cut short only when the file is exactly header-sized. A zero fill before the header, cut by a power failure, would leave a larger file of zeros that recovery would refuse as damaged. This is not hypothetical: the power-loss tests caught it when the exemption was removed (the mutant 'the header is preallocated too'). `FK_HEADER_SZ` is exported so the layer and the store cannot disagree about it.
- **A full disk does not fail a commit.** If the zeros cannot be written, the layer cuts back what it wrote, stops extending, and writes plainly, so it never fails a write the file itself would have accepted.
- **Compaction** writes its output to the sibling file as it is (it is written once, in bulk); after the rename the layer starts again from the new file's size.
- **What it costs.** Every byte of log is written twice, once as zeros. After an open the first extension is paid by the first commit.

Tested at every cut point: the power-loss harness can run every store behind the layer (`CRASH_PREALLOC=16384`; the zero fills and their syncs are I/O events like any other, with garbage or zeros possible in what they leave), the real-file tests check the file lengths, a crash without the trim, compaction, and the full-disk fallback, and the VFS power-loss sweep runs once behind the layer. Six mutants are caught: the header preallocated, a zero fill starting at the start of the file, a zero fill running after the data is written, the state forgotten after a compaction, a trim one byte too short, and a failed zero fill failing the write.

## In-memory index

Rebuilt by the same scan. Each branch has a head layer; a layer maps page number to the offset of its PAGE record (or a tombstone, for pages removed by shrinking). A layer is an **open-addressing hash table** (linear probing, never more than half full, keyed by page number), so its memory is proportional to the number of pages it holds, not to the highest page number it touched: a fork that writes one page near the end of a 1 GiB database costs a few hundred bytes instead of 2 MiB (500 branches that each wrote 1 MiB of random pages of a 1 GiB database: 1,007 MiB of process memory before, 15 MiB now). Entries cost 12 bytes in a table that is a quarter to half full, so 24 to 48 bytes per page held; a layer that holds every page of a database costs about three times what a dense array would (6 MiB instead of 2 MiB per GiB), which I accepted for a layer holding all pages of a database. A shrink adds a tombstone only for pages that exist below the layer. Compaction sorts a layer's entries by page to plan the new file; the snapshot format already listed pages sparsely, so it did not change. Forking freezes the source's head and gives both sides an empty one on top (O(1); an unchanged head is shared instead of stacked). A read walks the layer chain, then reads and checks one record. Chains only get shorter at compaction (below); between compactions a long fork history makes reads walk more layers.

## Keeping the layer tree small

Layers form a tree: a fork freezes the source's head layer, and the source and the new branch each get an empty layer on top of it. Two rules keep the tree as small as it can be, and are applied whenever a branch is discarded (live, and when the log is replayed):

1. **A layer with no live branch on it and no child is dead and is freed**, and so on up its parents.
2. **A layer with no live branch on it and exactly one child is folded into that child**: the two become one layer. The smaller layer's entries move into the larger one (the child's entry for a page wins, the parent's is used where the child has none, tombstones are carried like any other entry; the size is the child's). Reads give the same result before and after.

So with n live branches there are at most 2n - 1 layers, however many forks and discards there have been, and a read looks through at most that many. The power-loss and logic tests check that bound after every operation. A branch's head layer can change (when the parent side is the larger, it is the parent layer that survives and takes over the branch), which is invisible outside the store.

A fold that runs out of memory leaves a valid tree, because every entry copied so far is one the reader would have found anyway; the layers just stay unfolded until a later discard reaches them or a compaction folds them, so the bound above holds only while allocations succeed. The log is not affected: it still holds the pages of discarded branches until compaction.

**Open-time check-point.** When a store opened from a log (not a snapshot) is larger than 16 MiB, `fk_open` compacts it to a fresh file before returning, if the I/O layer supports sibling operations. The next open then loads from the snapshot instead of replaying every record. The check-point is best-effort: it never fails the open. A store that was already a snapshot at open time or whose caller lacks sibling I/O skips the check-point and works as before.

**Compaction still folds chains of frozen layers** (it keeps one layer per branch head and merges the layers in between), but since discards now fold eagerly it no longer meets any such chain, and **no test exercises that path**: instrumenting the logic pass showed 0 of 3,869 compactions met one, and the mutant that used to guard it ("pages of a folded layer are lost") became equivalent and was retired. The code is kept because a snapshot or log from before this change can still describe such a chain.

**Cost.** A fold copies the smaller layer's entries, so a loop of "write on main, fork, write on the fork, discard" costs time proportional to what main wrote in that round, not to everything in main. Measured on the benchmark machine (one run each, WSL2): on a 256 MiB database, 3,000 such rounds leave 1 layer and a cached read of main at 1.5 to 1.8 microseconds throughout and the index at 1.50 MiB, where before this change the same loop grew a layer per round, a read from 1.6 to 16.4 microseconds and the index to 2.4 MiB until a compaction. The log still grows (259 to 295 MiB for 259 MiB of live data) until compaction.

**Snapshots.** Compaction numbers its layers parent-before-child by walking the tree, because the array of layers is not in creation order once layers are dropped (a bug the first version of this change had, found by the logic pass: compaction failed with `FK_ECORRUPT` after discards).

## Branch ids

Branch 0 always exists. A fork is given an id below `FK_MAX_BRANCHES` (1,024): an id that was never used, or, from format version 3, **the id of a discarded branch**, so a workflow that forks and discards for every test run does not run out. The search is **next fit**: it starts just after the id the latest fork was given and wraps round (skipping 0), so a discarded id is the last one to come back, not the first. A caller holding a stale id (one it discarded) therefore has to survive a whole round of the id space before it is fooled. That is a mitigation, not a guarantee: **the caller must drop its handle when it discards a branch**, because a later fork may be given the same number, and a stale handle would then read the new branch. With every id alive, `fk_fork` returns `FK_ELIMIT`.

The rule depends only on the alive flags and a cursor (the id of the latest fork), so recovery reproduces it: replaying a FORK record sets the cursor to its id, and a version 3 snapshot ends with the cursor. A FORK record in a version 3 file is valid if its id is the next unused one or belongs to a discarded branch; in a version 1 or 2 file only the next unused one (those files never reuse ids). A version 2 file is only upgraded to version 3 by compaction, which writes a new file with a current header, so an older program is never given a log it would misread: it refuses the new version instead.

I considered the alternative of never reusing ids and lifting the limit instead (ids up to 2^32, 16 bytes in memory and 5 bytes in every snapshot for each id ever used). It has no stale-handle problem at all, but a store that has seen a million forks carries a million dead entries forever. Reuse with next fit keeps the table at its fixed size; if the stale-handle risk turns out to matter in practice (the layer above will keep names, not ids), the generation-number version of this is the next step.

## State ids, metadata and reset (format version 4)

Three things the layer above needs and could not build on the store as it was (see [SESSION-DESIGN.md](SESSION-DESIGN.md) for why).

**State ids** (`fk_state`). A number per live branch that says which content it holds. A commit gives the branch the next number from a store-wide counter; a fork or a reset gives the branch the number of its source. So two live branches hold the same content exactly when their ids are equal, as far as the store is concerned: an id is only ever given to one content, so equal ids mean equal content. The converse does not hold: a commit that writes back the bytes that were there gets a new id. The design note said a reset *replaces* the id; it *copies* the source's, because the id has to say "same content" and after a reset it is the same content. A counter is used, not the record's sequence number, because sequence numbers start again at 1 after a compaction. The counter and the ids are kept by recovery and by compaction. They mean nothing across stores, and a snapshot written by version 3 has none: when such a file is first compacted every live branch gets an id of its own (so equal content in two branches is no longer recognised, which only loses a fast path).

**Metadata** (`fk_meta_set`, `fk_meta_get`, `fk_fork_meta`). Up to `FK_META_MAX` (512) bytes per branch for the layer above to keep a name, a kind, an origin in. Replaced whole (length 0 clears it), one record and one fsync. `fk_fork_meta` forks and sets it in the same record, so there is no state in which the new branch exists without its label. Kept by recovery and by compaction (the snapshot carries it), and dropped with the branch, so a branch that later gets the same id starts without. A plain fork starts without metadata, whatever its source has. There is no parent pointer: ids are reused (see Branch ids), so a stored parent id could silently name a different branch later; the layer above records the origin in the metadata, together with whatever it needs to recognise the branch.

**Reset** (`fk_reset(dst, src)`). `dst` takes the content, the size and the state id of `src`, which stays as it is; `dst`'s metadata is kept; `dst` may be branch 0 (that is the fast-forward promote). One record and one fsync, O(1): the same two steps as a fork, but the new head layer goes to an existing branch, and the layers `dst` was on are released the way a discard releases them (freed, or folded into their only child). What `dst` held before is garbage for the next compaction. Like fork and discard it needs no open transaction (`FK_EBUSY` otherwise). **The store cannot know whether anything above still caches the old pages of `dst`**: the caller has to make sure of that (the VFS will refuse a reset while a connection is open on `dst`, because SQLite decides whether its page cache is stale from a counter that can repeat after a reset; that is reasoned from how SQLite works, not tested).

**Records.** `FORKM` (source, new id, length, metadata), `RESET` (destination, source) and `META` (branch, length, metadata) are one record each. A FORKM follows the id rule of FORK. A record that breaks its rules (a dead branch, a length over the limit) ends the valid prefix like any other bad record, and none of the three is accepted in a file below version 4. The snapshot of a version 4 file ends with the state counter and, for every branch id, its state id and metadata (zero and nothing for a dead id).

**Versions.** A new file is version 4. A version 3 file opens and works as before; `fk_version` says which the open file is; `fk_state` and `fk_meta_get` work on it too (ids as the replay of its log gives them, no metadata); `fk_reset`, `fk_meta_set` and `fk_fork_meta` return `FK_EVERSION` and change nothing. `fk_compact` writes the current format, so a file is upgraded by the same crash-safe rename as any compaction, and from then on an older program refuses it (`FK_ECORRUPT`, nothing touched), as it refuses any version it does not know.

## Running without syncs (`FK_FILE_NOSYNC`)

A store can be opened with the flag `FK_FILE_NOSYNC` (`fkx_options.nosync`, or `forkdb_init(path, root, 'nosync')` in SQL). The store then issues no `fsync`, `fdatasync`, `FlushFileBuffers` or directory sync for the life of that open, compaction included. It is for stores that are throwaway by design: a database per test, a build cache. Measured on ext4 with a 20 000-row seed, a fork, a ten-row update, a commit and a discard cost 8.5 ms with syncs and 0.07 ms without (a fork and a discard are one fsync each).

What it keeps and what it gives up:

- **The death of the process loses nothing.** The writes still reach the operating system, so a process that is killed, crashes or aborts leaves exactly the data it had acknowledged, with the machine up. Tested: a child killed with SIGKILL after 50 commits (and in the middle of an uncommitted transaction), a Python process that calls `os._exit` after 20 commits, and, in the power-loss harness, a replay of every workload from the volatile image.
- **A power cut or kernel crash can lose commits, and can make the file refuse to open.** Nothing orders the writes any more. The harness (`test/crash_test.c`, the "no-sync pass") cuts the power at a sample of the I/O events of seeded workloads, with a model in which nothing is ever durable and any subset of the writes, in any order, torn or not, may survive, plus the images where the first m writes survived in order. In 151 504 images (10 seeds of 8 operations) the open either **refused** (14 657) or returned **a state the workload had acknowledged** (15 587; in 14 207 of them an older one than the last acknowledged, in 10 700 an empty store), and the store was usable afterwards. Never a state that was not one of the workload's. That is the whole claim: *a state the workload acknowledged, or a refusal*. It is not "the last acknowledged state".
- **Nothing marks the file.** The next open, with or without the flag, reads it as usual. Only the open that used the flag skips the syncs, so the same store can be used durably later, and what was written without syncs is only as durable as the last sync made after it.
- **Not tested on real power cuts.** The claim rests on the simulator's model, which is deliberately pessimistic but is still a model. No real disk was power-cycled for either mode.
- **The lock still applies**, and the compaction temporary file inherits the flag.

How it is tested: `test/nosync_test.c` counts the real `fsync` and `fdatasync` calls (the linker's `--wrap`) of a durable open and of a nosync open through creation, commits, forks, discards, resets, compaction and close, with and without the preallocation layer: at least 20 against exactly 0. `test/fsync_count.c` does the same through the extension in Python, with an `LD_PRELOAD` interposer. The mutant `FK_BUG_NOSYNC_STILL_SYNCS` makes the flag a no-op and is caught.

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
- Recovery treats a snapshot as "replace the index with this". A snapshot is only accepted as the first unit of a version 2, 3 or 4 file (a version 3 snapshot ends with the allocation cursor, see Branch ids; a version 4 snapshot goes on with the state counter and each branch's state id and metadata), every offset in it must point at a CPAGE of that unit for the right page, and its checksum must match. The new file's record chain starts again at sequence 1.

**Errors.** Before the rename, any error (cannot create the temporary file, a write fails because the disk is full, an fsync fails) deletes the temporary file and returns the error; the store's file is untouched and the store stays usable. After the rename, an error is fail-stop like everywhere else: close and reopen.

**Cost and limits.** It rewrites all live data, so run it when there is a lot of garbage (hence the threshold), not on a schedule. The writer is blocked for the whole time; there is no online compaction. Peak disk use is the old file plus the live data; the store does not check free space first, it simply fails cleanly when the disk fills. I have not measured how long it takes on a large database and will not quote numbers before I have.

## How it was tested

The point of this work was to test the claim "nothing partial after a power cut" harder than the claim itself, so the harness is deliberately pessimistic. [test/simdisk.c](../test/simdisk.c) models a file with a volatile write cache:
- writes and truncations are **pending** until an fsync makes them durable;
- at a power cut the surviving image is the durable data plus **any subset** of the pending operations, applied in **any order**; a surviving write may be **torn** (any subset of its 512-byte sectors, or a byte prefix); the gap before a write that extends the file may hold zeros or garbage;
- an fsync can also fail, leaving everything pending;
- there are **two files**, the store's and the temporary one compaction writes, and **directory state**: a rename is visible at once but durable only after the directory sync. At a power cut a pending rename may or may not have happened, and the surviving file is resolved like any other (durable data plus any subset of its pending writes). The simulator allows one outstanding rename.

[test/crash_test.c](../test/crash_test.c) runs seeded random workloads (commits of 1-3 pages that can grow or shrink the database, forks, discards, **compactions**, up to 8 branches with **only 8 branch ids**, so ids are reused all the time, and up to 20 distinct pages, so that a layer's index table grows several times) against an oracle model that predicts which id every fork must get. For **every I/O event** of every workload it cuts the power just before the event, builds the post-crash images (all subsets when at most 8 operations are pending, a random sample of 130 otherwise, each both clean and torn), and checks that:
1. the store **opens**;
2. the recovered state equals the oracle after exactly the acknowledged operations, or those plus the one in flight (every branch, every page, every size);
3. the store is **usable afterwards**: the rest of the workload is replayed on it, then it is closed, reopened and compared with the oracle's final state;
4. **a power cut during recovery** itself (at each of its I/O events, with the same variety of images) is also survivable;
5. after a **failed fsync** the store is fail-stop, and the same checks hold for the disk it leaves behind. The one exception is a compaction that fails before the rename (its fsync is on the temporary file): the store must stay usable, and it is checked to;
6. after every compaction, the **in-memory state still equals the oracle**, the file holds exactly the live data, and compacting again changes nothing.

Cutting the power before every I/O event of a compaction (creating the file, each page, each snapshot chunk, the marker, the fsync, the rename, the directory sync) is part of 1-4: the oracle does not change across a compaction, so the recovered state must be the same either way, whichever file survived.

The power cuts are expensive, so only about one workload in a few contains a compaction. A second, cheap pass therefore runs thousands of **longer workloads with several compactions each and no power cut**, checking the oracle after every operation and again after a close and reopen. It is what exercises layer folding and tombstone handling at volume.

The oracle also tracks, per branch, which content it holds (a class that a commit makes new and a fork or reset copies) and its metadata, and the workloads include resets, metadata changes and forks with metadata. After every operation and every recovery the store's state ids must be equal for exactly the pairs of branches the oracle says hold the same content, and the metadata must match byte for byte. [test/file_test.c](../test/file_test.c) opens a real version 3 file written by the previous store (`test/data/v3-compacted.fk`, with a snapshot and records after it), shows the refusals, upgrades it by compaction and checks that nothing was lost; exercises the three new calls and their errors on a version 4 store, across a close, a recovery and a compaction; and cuts the last three bytes off each of the three new records, written by a process that dies without closing, to show that recovery drops the torn record and the store is as it was before it.

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
| compaction: tombstones dropped where they hide a lower layer | no | **catches** | **catches** |
| compaction: copies a damaged page without checking | no | no | **catches** |
| open: a leftover compaction file is not deleted | no | no | **catches** |
| index: a rehash loses one entry (off-by-one in the loop) | **catches** | **catches** | **catches** (the first version of the harness used 6 distinct pages and **missed it**; with 20 pages the harness catches it, and the file tests with 3,000 pages do as well) |
| index: a collision is treated as a miss | **catches** | **catches** | **catches** |
| index: overwriting a page adds a second entry instead | **catches** | **catches** | **catches** |
| ids: fork can hand out the id of a live branch | **catches** | **catches** | **catches** |
| ids: recovery rejects a fork that reuses an id (still has the old rule) | **catches** | **catches** | **catches** |
| ids: the allocation cursor is not restored from a snapshot | **catches** | **catches** | **catches** |
| layers: a fold lets the parent's entry win over the child's | **catches** (together with the logic pass) | | **catches** |
| layers: a fold leaves the surviving layer with the wrong size | **catches** (together with the logic pass) | | **catches** |
| layers: a fold leaves a branch pointing at the freed layer | **catches** (together with the logic pass) | | **catches** |
| layers: a fold does not carry tombstones over | **catches** (with the logic pass; my quick 4-seed check missed it, the full run does not) | | **catches** |
| layers: a layer with two children is folded as well | **catches** (together with the logic pass) | | **catches** |
| layers: a layer other layers build on is freed on discard | **catches** (together with the logic pass) | | **catches** |
| layers: discard never frees layers | **catches** (the layer-count check, with the logic pass) | | **catches** |
| layers: discard never folds single-child layers | **catches** (the layer-count check, with the logic pass) | | **catches** |
| hardware CRC32C leaves out the last bytes of a buffer | no | no | **catches** (the CRC test; the store still works with itself, so nothing else notices) |
| v4: a commit leaves the state id as it was | **catches** | **catches** | **catches** |
| v4: a fork gets a state id of its own | **catches** | **catches** | **catches** |
| v4: a reset leaves the branch with its old content | no | **catches** | **catches** |
| v4: a reset gives the branch a state id of its own | **catches** | **catches** | **catches** |
| v4: the state counter is not restored from a snapshot (an id can be handed out twice) | **catches** | **catches** | **catches** |
| v4: a version 3 snapshot gives every branch the same state id | no | no | **catches** (only the real version 3 file shows it) |
| v4: a discarded branch leaves its metadata to the next one that gets its id | no | **catches** (ids are reused all the time) | no |
| v4: recovery forgets the metadata a fork was given | **catches** | **catches** | **catches** |
| v4: compaction leaves the metadata out | **catches** | **catches** | **catches** |

Which test catches which is not what I expected in two places, and I would rather say so than let the table suggest more than it shows:
- I expected the power-loss harness to catch the torn-tail and record-chain mutants too (stale records resurfacing after a torn tail); it did not, including a run of about 330 workloads of 12 operations (about 273,000 crash images). A stale record has to line up exactly with the new tail, which random workloads almost never produce. The two mechanisms also protect the same property, so either alone is enough in practice. The deterministic file tests are what show each one is needed.
- The two layer-folding mutants **survived the power cuts**: 15 workloads of 10 operations with 15 compactions in them, 0 violations. The cuts are expensive, so few workloads reach the particular shape (discard one side of a fork, then compact) that folding needs. That is why the logic pass exists: it found both within a handful of workloads, and a deterministic file test now covers each as well.
- "no" is a statement about this harness, not about the bugs.

## What this does not cover

- **The disk model is an assumption.** It assumes 512-byte sector atomicity, that fsync really makes data durable, and that data does not rot while the machine runs. A drive that lies about fsync, or a file system that loses a file's size update, is outside it. The directory entry of a newly created file is fsynced on the real backend but not modeled.
- **Compaction replaces the log atomically.** On POSIX with `rename`. On Windows with a rename made with POSIX semantics through the temporary file's handle, which then becomes the store's handle, so the one-opener lock is never released during the swap (see KNOWN-LIMITS.md for what it needs).
- **One process, one writer transaction at a time.** The file is locked while it is open (`flock`; on Windows the share mode does it), so a second process is refused with `FK_ELOCKED` instead of corrupting the file. The lock is released when the holder closes the file or dies. A filesystem that cannot lock (`ENOLCK`, `EOPNOTSUPP`: some network filesystems) is not refused, so there is no protection there. Before the lock existed, two processes writing one store lost acknowledged commits, and in 4 of 8 runs left a file that no longer opened.
- **Compaction blocks the writer** and rewrites all live data; it is not incremental and not online. A segmented log that reclaims cold segments one at a time would avoid both, at the price of a manifest for atomicity (roughly doubling the work); I would only move to it if whole-file rewrites turn out to hurt on real databases. I have not measured compaction time on a large database.
- **Nothing calls compaction automatically.** `fk_compact_if_needed` is there for the layer above to call (after a discard, say); the store itself never decides to.
- **Between compactions the log grows** (overwritten pages and the pages of discarded branches stay in it until `fk_compact`). The in-memory layer tree does not: see "Keeping the layer tree small".
- **The simulator models one outstanding rename**, and a rename that is atomic (as POSIX requires) but is lost or kept as a whole at a power cut. A file system that can leave a zero-length file after a rename without a prior fsync is covered (the mutant that skips the fsync shows it); one that breaks rename atomicity is not.
- **A tombstone-heavy shrink** is tested for correctness but not for cost.
- **Limits are small:** 1,024 branches alive at once (`FK_ELIMIT` when full), 2^28 pages per database, whole transactions held in memory until commit.
- **A shrink walks every page number it removes** (one table lookup each, so time is proportional to the size removed, though memory is not).
- **The store is not the SQLite VFS.** It is a page store with branches; the VFS in [vfs/](../vfs) sits on top of it, and has its own power-loss tests.
- Tests ran on one machine, on a real file only for a normal close, a process killed without closing (also in the middle of a compaction, before and after the rename), an appended torn tail, a spliced record, a damaged record, a damaged page met during compaction, a full disk (writes to the temporary file failing), and relabelled format versions. A real power cut was not performed.

## Running the tests

```sh
scripts/test-store.sh          # file tests, logic pass and power-loss tests under sanitizers, mutant checks
scripts/test-store.sh quick    # fewer workloads
SEEDS=300 OPS=10 scripts/test-store.sh
```

Needs only gcc.
