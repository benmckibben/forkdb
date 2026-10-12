# SQLite on the durable store: the VFS spike (2026-10-11)

Status: **spike**, experimental, not wired into anything. Code in [vfs/](../vfs), tests in [scripts/test-vfs.sh](../scripts/test-vfs.sh), raw results in [vfs/results/](../vfs/results). Every number comes from one machine (WSL2, ext4 on a virtual disk), three repetitions, SQLite 3.50.0; see the limits at the end.

## Verdict

The approach holds. SQLite runs on forkdb branches through a VFS over the durable store, and a commit is **one store commit and one fsync**, whether or not SQLite's batch-atomic path is used. Three things in it changed the plan:

1. **Batch-atomic writes are not needed.** The store's commit is already atomic, so the VFS keeps SQLite's rollback journal in memory and publishes the transaction's pages at `xSync`. That path measured the same as the batch-atomic path (table below), and it works on a stock SQLite build. The earlier plan ("ship a pinned SQLite build, make batch-atomic an optional optimisation") loses its first half for this reason; a pinned build is still needed if forkdb requires the session extension, which this spike did not test.
2. **WAL cannot be supported by this design as it is**, and one way of switching it on is a trap that a test found: see finding 3.
3. **A transaction is staged in RAM** and the store buffers it again at commit, so the transaction size is bounded by memory and peaks at about twice the dirty pages (finding 5).

## What was built

[vfs/fkvfs.c](../vfs/fkvfs.c), 663 lines of C, and [vfs/fkvfs.h](../vfs/fkvfs.h). A database is opened as `b<N>`, N being a branch id: `sqlite3_open_v2("b3", &db, flags, "forkdb")`.

- The main database file is the branch's pages in the store. Reads go to the store, or to the connection's own staged pages. Writes are staged in a per-connection hash table and **published as one store transaction**: at `COMMIT_ATOMIC_WRITE` on the batch path, at `xSync` on the journal path.
- Journals and temporary files are in memory and die with the process. This is safe because nothing reaches the store until the whole transaction is published, so a crash can never leave a half-applied transaction that a journal would have to repair.
- Publishing drops staged pages equal to the committed ones, and commits nothing if none are left. That is what makes a rolled-back transaction that spilled pages cost no write.
- Locks are in-process (per branch: shared, reserved, pending, exclusive), so several connections can use one branch, and a writer's commit waits for readers as in rollback-journal mode.
- `xFileControl` refuses two pragmas (findings 3 and 4). The database page size must be a multiple of 4096.
- A fork is `fk_fork` on the store, as before; the VFS does nothing for it. Connections already open on the parent keep working.

## What the tests show

`scripts/test-vfs.sh` builds the pinned SQLite twice (stock, and with `SQLITE_ENABLE_BATCH_ATOMIC_WRITE`), runs everything under address and undefined-behaviour sanitizers, then runs the mutant self-check.

- **Functional (T1 to T10):** persistence across a close and reopen; fork isolation in both directions, a fork of a fork, and discarding the middle branch; a refused batch commit falls back to the journal and the transaction still succeeds; a transaction bigger than the page cache, rolled back (no store commit, no size change) and committed; page sizes 4096, 8192 and 65536, with 1024 refused; all five rollback-journal modes; two connections on one branch (a second writer gets `SQLITE_BUSY`, a reader delays a commit); `VACUUM` and `auto_vacuum=FULL` shrinking; temporary files, big sorts and savepoints; bad names and dead branches. 271 checks on the batch build, 257 on the stock build, 0 failures, no sanitizer reports.
- **Power loss (T11):** SQLite on the existing `simdisk` model (a volatile write cache: any subset of unsynced writes may survive, in any order, torn). The workload is 7 transactions on branch 0 (a multi-page one every third), a fork after the third, and 4 more on the fork. The power is cut before every one of the 93 I/O events, and separately the fsync with each event number is made to fail (and the power then cut), then each post-crash image is opened and checked: `integrity_check`, no partial transaction, no row from the other branch, every acknowledged transaction present, and an acknowledged fork still there. **189 crash or failure points, 6,820 post-crash images per variant (batch path and journal path), 0 violations.**
- **Do the tests catch broken code? 14 of 14 mutants caught** (an atomic commit that discards its pages and says OK; half a transaction published; dedupe dropping everything or nothing; a forgotten shrink; no read-your-writes; a writer not held off by readers; a failed sync reported as OK; the two pragmas not refused; a truncate that keeps staged pages; a dead branch not refused; a no-op publish that leaves its staged pages in place, which Cursor's review found).
- **The mutants found four gaps in my own tests**, now fixed, and two of them were real design problems (findings 3 and 4). The first version of the gate let four mutants survive. A fifth gap was found in review, not by the gate: a rolled-back transaction that had spilled pages kept its staged buffers until the connection closed, and they were scanned again at every later lock release. Its test and the 14th mutant were added after that.

## Measurements

Same machine and disk for all columns, configurations interleaved, 3 repetitions: median, with the range in brackets. A 4 KiB write plus fsync on this disk takes **3.98 ms** (p50; 4.69 ms p99), so every number that is one fsync is about 4 ms. Rows are inserted as 100-byte random blobs; the database ends at 8,185 pages (33.5 MB).

| | plain file, rollback journal | plain file, WAL | forkdb, batch path | forkdb, journal path |
|---|---|---|---|---|
| single-row commits per second | 91.8 | 300.2 | 263.2 | 258.1 |
| single-row commit p50 / p99 (ms) | 10.9 / 14.8 | 3.20 / 5.02 | 3.99 / 5.85 | 4.09 / 6.17 |
| commit of 1000 rows, p50 / p99 (ms) | 11.4 / 14.8 | 3.62 / 15.8 | 4.52 / 6.48 | 4.53 / 6.14 |
| one transaction of 200,000 rows (s) | 0.150 | 0.173 | 0.165 | 0.183 |
| random primary-key read (us) | 8.75 | 3.49 | 3.30 | 3.53 |
| full scan of the table (ms) | 16.4 | 16.4 | 20.8 | 22.0 |
| first query after reopening (ms) | 7.1 | 7.7 | 12.0 | 13.7 |
| file size after the run (MB) | 33.5 | 33.5 | 52.1 (34.0 live) | 52.1 (34.0 live) |

Store only: fsyncs per single-row commit 1.03; bytes written per single-row commit 8,439; opening the 52 MB log 26 ms (batch run) and 29 ms (journal run); **fork 3.6 ms p50** (p99 5.1 ms) on the 33.5 MB database, against **38 to 46 ms to copy the file**; a one-row update on a fork 3.4 ms and 8,324 bytes of log.

What to read from it, and what not to:

- **A commit costs the fsync and little else.** 3.99 ms p50 against the 3.98 ms floor. That is 2.7 times faster than SQLite's default rollback journal (10.9 ms: about 2.7 fsyncs' worth, I did not count them) and 25% slower than WAL, which only has to append. The batch path and the journal path are within noise of each other everywhere (one transaction of 200,000 rows is the only place they differ, 0.165 against 0.183 s, and the ranges overlap).
- **The random-read result is not a merit of the store.** The plain-file rollback-journal column is slow because every statement takes and releases a file lock and checks the file header, which are syscalls; the VFS does that in process. The likely reason is not measured (I did not trace the syscalls). Against WAL, which avoids most of that, the two are equal.
- **Scans are 27% slower** (20.8 against 16.4 ms): every page goes through the layer lookup, a pread and a checksum. The data is in the OS cache; a cold read is not measured.
- **Fork time is one fsync** (3.6 ms here), independent of the database size by construction, and the log grows 40 bytes. It was measured at one size only; the 38 to 46 ms for the copy is for the same 33.5 MB.
- **Write amplification is real:** a single-row commit writes 8.4 KB, two pages and the record, because SQLite rewrites page 1 (the file change counter) on every commit. After the whole run the log was 18 MB larger than the live data (52.1 against 34.0); the 2,000 single-row commits alone wrote 16.9 MB. Compaction (already built) is what reclaims it, and nothing triggers it yet.
- Absolute values will move on a disk whose fsync is not 4 ms. The ratios that are fsync counts (rollback journal against forkdb against WAL) should survive; the ones that are CPU (scan, random read) should not move much. **Not measured.**

## Findings

1. **Batch-atomic is redundant here** (above). SQLite also does not use it for the first transaction on an empty database, which takes the journal path even when the VFS advertises it (T1 shows 0 batch commits for the `CREATE TABLE`), so the journal path has to be correct on its own, and the power-loss sweep checks it.
2. **Publishing at `xSync` is the commit point, and `xSync` is also what reports failure.** A failed publish keeps the staged pages, so a later `xSync` retries and surfaces the error. The first version of the mutant "atomic commit failed but reported OK" survived for that reason; the strengthened mutant also drops the pages and is caught.
3. **WAL: refused in two places, and the second one is a trap.** `xOpen` refuses a `-wal` file, but with `PRAGMA locking_mode=EXCLUSIVE` SQLite allows WAL without shared memory, writes WAL (version 2) into the database header **and answers `wal`** before it opens the `-wal` file. A branch could be left claiming WAL while the VFS refuses WAL, and would not open again. The pragma is therefore intercepted in `xFileControl` before SQLite acts on it, and a test checks that header bytes 18 and 19 stay 1 and that the branch reopens.
4. **`synchronous=OFF` is refused.** SQLite then never calls `xSync`, so a commit on the journal path is published when the write lock is dropped, and unlock has no way to report an error. The sweep showed acknowledged transactions lost, starting with the very first transaction (which takes the journal path, finding 1). The VFS publishes at unlock only for the rollback-after-spill case now, where equal pages are dropped and nothing is written; the tests assert that no publish failed silently.
5. **The VFS stages a transaction's pages in RAM, and `fk_txn_write` copies them again** into the store's own buffer until the commit. A transaction can be no bigger than about half the memory available, and a long one costs memory for its whole life. A store call to read a page from the open transaction would let the VFS stage nothing. Not measured; reasoned from the code.
6. **Page size must be a multiple of 4096.** The store has 4096-byte pages and records sizes in pages, so a 1024-byte SQLite page cannot be represented. `PRAGMA page_size=1024` followed by a write fails with `SQLITE_IOERR` ("disk I/O error"), which is a poor message; intercepting the pragma would give a clear one. The default page size is 4096, so ordinary use never sees it.
7. **A crash needs no recovery in the VFS.** The journal is in memory and the store commit is atomic, so after a restart there is never a hot journal to replay: the database is the last committed state. A rolled-back or abandoned transaction is just the staged pages dropped.

## Limits: not tested, or only reasoned

- **One machine, three repetitions, a 4 ms fsync.** No NVMe or SSD. No cold-cache reads.
- **The power-loss workload is small**: 11 transactions and a fork. The functional tests cover spilled transactions, `VACUUM`, truncation and temporary files, but not under a power cut. The model is `simdisk`'s, with its limits (see DURABLE-STORE.md).
- **Single process.** Locks are in-process and the store is single-writer. Two processes on one store file are not supported and not tested.
- **Not tested:** the session extension through this VFS (diff, promote, undo); `ATTACH` of two branches in one transaction, which commits each file separately and is **not atomic across branches** (reasoned: there is one store commit per file, and nothing like a super-journal for it); discarding a branch that has a connection open (ids are reused, as DURABLE-STORE.md warns: close connections first); concurrency stress; databases over 1 GB; any real application.
- **The mutant list is not proof.** 14 of 14 caught means these 14 faults are caught.

## Reproduce

```sh
scripts/test-vfs.sh                  # both SQLite builds under the sanitizers, then the mutants (a few minutes; the first run compiles SQLite twice)
scripts/test-vfs.sh quick            # same, a sparse power-loss sweep, no mutants
vfs/build.sh bench batch && vfs/build/vfs_bench-batch /some/dir 3
```

Needs gcc, curl, sha256sum and python3. Run the benchmark on the disk you care about, not in `/tmp` if that is a RAM disk.

## Suggested next steps (for you to pick from)

1. Drop batch-atomic from the design (DESIGN.md consequence 1) and say so in the README.
2. A store call to read a page of the open transaction, so the VFS stops staging (finding 5). A small store change with its own tests.
3. Run the same benchmark on a real SSD or NVMe.
4. The session extension on top of the VFS: diff, promote and undo on a branch.
5. The open-time checkpoint in the log and an automatic compaction trigger, now that a VFS user would feel both (write amplification above; open time 26 ms for 52 MB here).
