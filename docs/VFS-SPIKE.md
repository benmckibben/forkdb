# SQLite on the durable store: the VFS spike (2026-10-11)

Status: **spike**, experimental, not wired into anything. Code in [vfs/](../vfs), tests in [scripts/test-vfs.sh](../scripts/test-vfs.sh), raw results in [vfs/results/](../vfs/results). Every number comes from one machine (WSL2, ext4 on a virtual disk), three repetitions, SQLite 3.50.0; see the limits at the end.

## Verdict

The approach holds. SQLite runs on forkdb branches through a VFS over the durable store, and a commit is **one store commit and one fsync**, whether or not SQLite's batch-atomic path is used. Three things in it changed the plan:

1. **Batch-atomic writes are not needed.** The store's commit is already atomic, so the VFS keeps SQLite's rollback journal in memory and publishes the transaction's pages at `xSync`. That path measured the same as the batch-atomic path (table below), and it works on a stock SQLite build. The earlier plan ("ship a pinned SQLite build, make batch-atomic an optional optimisation") loses its first half for this reason; a pinned build is still needed if forkdb requires the session extension, which this spike did not test.
2. **WAL cannot be supported by this design as it is**, and one way of switching it on is a trap that a test found: see finding 3.
3. **A transaction is staged in RAM**, so its size is bounded by memory. The spike held each page twice until the commit; the store call `fk_txn_write_owned` (added after the spike) hands the staged buffer to the store without a copy, which halved the peak (finding 5).

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

- **Functional (T1 to T10):** persistence across a close and reopen; fork isolation in both directions, a fork of a fork, and discarding the middle branch; a refused batch commit falls back to the journal and the transaction still succeeds; a transaction bigger than the page cache, rolled back (no store commit, no size change) and committed; page sizes 4096, 8192 and 65536, with 1024 refused; all five rollback-journal modes; two connections on one branch (a second writer gets `SQLITE_BUSY`, a reader delays a commit); `VACUUM` and `auto_vacuum=FULL` shrinking; temporary files, big sorts and savepoints; bad names and dead branches. 335 checks on the batch build, 319 on the stock build, 0 failures, no sanitizer reports.
- **Power loss (T11):** SQLite on the existing `simdisk` model (a volatile write cache: any subset of unsynced writes may survive, in any order, torn). The workload is 7 transactions on branch 0 (a multi-page one every third), a fork after the third, and 4 more on the fork. The power is cut before every one of the 93 I/O events, and separately the fsync with each event number is made to fail (and the power then cut), then each post-crash image is opened and checked: `integrity_check`, no partial transaction, no row from the other branch, every acknowledged transaction present, and an acknowledged fork still there. **189 crash or failure points, 6,820 post-crash images per variant (batch path and journal path), 0 violations.**
- **Do the tests catch broken code? 16 of 16 mutants caught** (an atomic commit that discards its pages and says OK; half a transaction published; dedupe dropping everything or nothing; a forgotten shrink; no read-your-writes; a writer not held off by readers; a failed sync reported as OK; the two pragmas not refused; a truncate that keeps staged pages; a dead branch not refused; a no-op publish that leaves its staged pages in place, which Cursor's review found; a failed commit that keeps staging whose pages the store already took, added with `fk_txn_write_owned`; a zero-fill pass that overwrote pages already handed to the store, which Cursor's review of the ownership change found: after `stage_take` the buffer is NULL but the key stays, and the loop tested the buffer, so a page written back into a truncated range was committed as zeros. Test T13 and the 16th mutant guard it).
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

- **A commit costs the fsync and little else.** 3.99 ms p50 against the 3.98 ms floor. That is 2.7 times faster than SQLite's default rollback journal (10.9 ms: about 2.7 fsyncs' worth, I did not count them) and 25% slower than WAL, which only has to append. The batch path and the journal path are within noise of each other everywhere (one transaction of 200,000 rows is the only place they differ, 0.165 against 0.183 s, and the ranges overlap). **Correction, 2026-10-12: that is how it looks on this WSL2 disk only. On real disks forkdb is about 3 times slower than WAL (see Real disks below).**
- **The random-read result is not a merit of the store.** The plain-file rollback-journal column is slow because every statement takes and releases a file lock and checks the file header, which are syscalls; the VFS does that in process. The likely reason is not measured (I did not trace the syscalls). Against WAL, which avoids most of that, the two are equal.
- **Scans are 27% slower** (20.8 against 16.4 ms): every page goes through the layer lookup, a pread and a checksum. The data is in the OS cache; a cold read is not measured.
- **Fork time is one fsync** (3.6 ms here), independent of the database size by construction, and the log grows 40 bytes. It was measured at one size only; the 38 to 46 ms for the copy is for the same 33.5 MB.
- **Write amplification is real:** a single-row commit writes 8.4 KB, two pages and the record, because SQLite rewrites page 1 (the file change counter) on every commit. After the whole run the log was 18 MB larger than the live data (52.1 against 34.0); the 2,000 single-row commits alone wrote 16.9 MB. Compaction (already built) is what reclaims it, and nothing triggers it yet.
- Absolute values will move on a disk whose fsync is not 4 ms. The ratios that are fsync counts (rollback journal against forkdb against WAL) should survive; the ones that are CPU (scan, random read) should not move much. **Not measured.** **That prediction was wrong for WAL: the ratios did not survive (see Real disks below). The CPU-bound ones did.**

## Real disks (2026-10-12)

One throwaway `i4i.large` (Xeon Platinum 8375C, Amazon Linux 2023, kernel 6.1.188, SQLite 3.50.0) built from `infra/bench` ([README](../infra/bench/README.md)), running the same `vfs_bench`, 3 repetitions on each of two disks, both ext4 with default options: the **local NVMe instance store** and a **gp3 EBS volume** (3000 IOPS, 125 MB/s). Medians, ranges are in [vfs/results/bench-real-disks.txt](../vfs/results/bench-real-disks.txt). The same 335 and 319 checks of `scripts/test-vfs.sh quick` also passed on that host (0 failures), so the VFS tests hold on a second kernel and filesystem.

| single-row commit | rollback journal | WAL | forkdb (batch path) | forkdb (journal path) |
|---|---|---|---|---|
| NVMe: p50 (us) | 250.8 | 44.7 | 149.6 | 153.1 |
| NVMe: commits per second | 3,755 | 11,205 | 6,396 | 6,267 |
| EBS gp3: p50 (us) | 6,537 | 992 | 2,839 | 2,870 |
| EBS gp3: commits per second | 155 | 637 | 418 | 345 |

| | NVMe: rollback journal | WAL | forkdb (batch) | EBS: rollback journal | WAL | forkdb (batch) |
|---|---|---|---|---|---|---|
| 1000-row commit p50 (ms) | 1.07 | 0.71 | 0.90 | 10.0 | 2.74 | 4.59 |
| 200,000 rows in one transaction (s) | 0.135 | 0.167 | 0.150 | 0.160 | 0.198 | 0.166 |
| random primary-key read (us) | 4.11 | 2.21 | 2.96 | 4.10 | 2.07 | 2.90 |
| full scan (ms) | 15.7 | 16.4 | 21.9 | 16.3 | 14.9 | 22.2 |
| copy of the database file (ms) | 31.6 | 32.0 | n/a | 56.0 | 55.4 | n/a |

Store only: **fork 97.8 us p50 on NVMe (p99 170 us) and 2.53 ms on EBS**, against 31.6 ms and 56 ms to copy the 33.5 MB file; a one-row update on a fork 271 us on NVMe, 8,324 bytes of log; 1.03 fsyncs and 8,439 bytes written per single-row commit; opening the 52 MB log 18.7 ms, and the first count after reopening 12.1 ms. A 4 KiB write plus fsync on this host: 132 us p50 on NVMe, 2.79 ms on EBS.

What changed, what did not:

- **forkdb is about 3 times slower than WAL at a single-row commit on real disks** (149.6 against 44.7 us on NVMe, 2.84 against 0.99 ms on EBS), not 25% slower as the WSL2 numbers suggested. It is still faster than SQLite's default rollback journal: 1.7 times on NVMe, 2.3 times on EBS. For larger transactions the gap to WAL is smaller (1000 rows: 1.3 times on NVMe, 1.7 times on EBS) and a 200,000-row transaction is within noise of both.
- **The cause of the gap to WAL is not known.** The store already uses `fdatasync`, so it is not fsync against fdatasync, and it does one sync per commit like WAL. The WAL commit on EBS (0.99 ms) is also faster than my own 4 KiB write plus fsync loop (2.79 ms), so that loop is not a floor for a commit: it extends the file and calls `fsync`. Differences I have not tested: the log grows 8.4 KB on every commit where a WAL is reused after each checkpoint, and the VFS writes two pages and a record where WAL appends frames. **This is a hypothesis list, not a finding.** A measurement that varies one of them (preallocating the log, or counting the I/O with `strace`) would say which.
- **The WSL2 prediction that the ratios between configurations are fsync counts was wrong** and is corrected above. The CPU-bound results held up: the scan is 34 to 49 percent slower than a plain file (21.9 against 15.7 and 16.4 ms), identical on both disks, so it is the layer lookup, pread and checksum, not the disk.
- **The random-read result moved.** On WSL2 forkdb looked faster than the plain file with a rollback journal (3.3 against 8.75 us); here it is 2.96 against 4.11 us, still faster than that configuration and about 30 percent slower than WAL (2.21 us).
- **Fork cost scales with the disk's sync time**, as predicted: one sync, no data copied. On NVMe a fork is about 300 times cheaper than copying this database; on EBS about 22 times.
- **Write amplification and open time are as before** (8.4 KB per commit; the log is 52.1 MB for 34.0 MB live; 18.7 ms to open it, which is the quantity that grows at about 3 s per GiB of log).

Limits of this run: one instance of one type in one availability zone, three repetitions each (the ranges are in the results file and are small), a database of 33.5 MB that fits in memory, no cold-cache reads, one filesystem (ext4, default options). The first attempt at this run lost its output (the console of a terminated instance is empty), so the numbers come from the second run.

## Findings

1. **Batch-atomic is redundant here** (above). SQLite also does not use it for the first transaction on an empty database, which takes the journal path even when the VFS advertises it (T1 shows 0 batch commits for the `CREATE TABLE`), so the journal path has to be correct on its own, and the power-loss sweep checks it.
2. **Publishing at `xSync` is the commit point, and `xSync` is also what reports failure.** A failed publish returns the error from `xSync`, and since the store call that took over the staged pages (finding 5) it drops the staging too, because the pages already handed over are freed by the store's abort; SQLite rolls the transaction back after a failed `xSync` or atomic commit, and the rollback rewrites the old pages, which equal the committed ones and publish nothing. The first version of the mutant "atomic commit failed but reported OK" survived for that reason; the strengthened mutant also drops the pages and is caught.
3. **WAL: refused in two places, and the second one is a trap.** `xOpen` refuses a `-wal` file, but with `PRAGMA locking_mode=EXCLUSIVE` SQLite allows WAL without shared memory, writes WAL (version 2) into the database header **and answers `wal`** before it opens the `-wal` file. A branch could be left claiming WAL while the VFS refuses WAL, and would not open again. The pragma is therefore intercepted in `xFileControl` before SQLite acts on it, and a test checks that header bytes 18 and 19 stay 1 and that the branch reopens.
4. **`synchronous=OFF` is refused.** SQLite then never calls `xSync`, so a commit on the journal path is published when the write lock is dropped, and unlock has no way to report an error. The sweep showed acknowledged transactions lost, starting with the very first transaction (which takes the journal path, finding 1). The VFS publishes at unlock only for the rollback-after-spill case now, where equal pages are dropped and nothing is written; the tests assert that no publish failed silently.
5. **The VFS stages a transaction's pages in RAM; it used to hold each of them twice until the commit.** `fk_txn_write` copies a page into the store's own buffer, so the peak was about twice the dirty pages. `fk_txn_write_owned` now takes the staged buffer itself (it frees it at commit or abort; on an error the caller still owns it), and the VFS hands its staged pages over one by one. Measured on this machine, stock SQLite, journal path, one transaction inserting 100-byte blobs, peak resident set from `getrusage`: **200,000 rows 48.9 MB before, 27.0 MB after; 1,000,000 rows 225.3 MB before, 116.4 MB after** (three runs each at 1,000,000 rows, the same values to within 0.2 MB). That is about 1.9 times the dirty data before and 1 times after. The staging itself remains: a transaction is still bounded by memory, at about the size of the pages it dirties. Holding a store transaction open across the whole SQLite transaction and writing into it directly was considered and rejected: the store has one open transaction at a time across all branches, and `fk_fork`, `fk_discard` and `fk_compact` return `FK_EBUSY` while it is open, so a long SQLite write would block forks, discards, compaction and writers on every other branch.
6. **Page size must be a multiple of 4096.** The store has 4096-byte pages and records sizes in pages, so a 1024-byte SQLite page cannot be represented. `PRAGMA page_size=1024` followed by a write fails with `SQLITE_IOERR` ("disk I/O error"), which is a poor message; intercepting the pragma would give a clear one. The default page size is 4096, so ordinary use never sees it.
7. **A crash needs no recovery in the VFS.** The journal is in memory and the store commit is atomic, so after a restart there is never a hot journal to replay: the database is the last committed state. A rolled-back or abandoned transaction is just the staged pages dropped.

## Limits: not tested, or only reasoned

- **One machine, three repetitions, a 4 ms fsync** for the numbers above. Real disks are measured in the next section, on one instance of one type. No cold-cache reads anywhere.
- **The power-loss workload is small**: 11 transactions and a fork. The functional tests cover spilled transactions, `VACUUM`, truncation and temporary files, but not under a power cut. The model is `simdisk`'s, with its limits (see DURABLE-STORE.md).
- **Single process.** Locks are in-process and the store is single-writer. Two processes on one store file are not supported and not tested.
- **Not tested:** the session extension through this VFS (diff, promote, undo); `ATTACH` of two branches in one transaction, which commits each file separately and is **not atomic across branches** (reasoned: there is one store commit per file, and nothing like a super-journal for it); discarding a branch that has a connection open (ids are reused, as DURABLE-STORE.md warns: close connections first); concurrency stress; databases over 1 GB; any real application.
- **The mutant list is not proof.** 16 of 16 caught means these 16 faults are caught. Adding `fk_txn_write_owned` showed how a mutant can be caught by accident: "a truncate keeps the staged pages beyond it" had been caught only because a failed publish used to keep its staging and fail a second time at unlock, where a counter saw it. With the staging now dropped on failure, SQLite swallowed the first failure during a rollback and the mutant survived until the test counted failures at `xSync` as well (`sync_publish_errors`). Test T12 injects a publish that fails part way (`fkvfs_fail_publish_page`) to cover the new failure path.

## Reproduce

```sh
scripts/test-vfs.sh                  # both SQLite builds under the sanitizers, then the mutants (a few minutes; the first run compiles SQLite twice)
scripts/test-vfs.sh quick            # same, a sparse power-loss sweep, no mutants
vfs/build.sh bench batch && vfs/build/vfs_bench-batch /some/dir 3
```

Needs gcc, curl, sha256sum and python3. Run the benchmark on the disk you care about, not in `/tmp` if that is a RAM disk.

## Suggested next steps (for you to pick from)

1. ~~Drop batch-atomic from the design (DESIGN.md consequence 1) and say so in the README.~~ Done, in the follow-up docs PR.
2. A store call to read a page of the open transaction, so the VFS stops staging (finding 5). A small store change with its own tests.
3. ~~Run the same benchmark on a real SSD or NVMe.~~ Done (below). It changed one conclusion.
4. The session extension on top of the VFS: diff, promote and undo on a branch.
5. The open-time checkpoint in the log and an automatic compaction trigger, now that a VFS user would feel both (write amplification above; open time 26 ms for 52 MB here).
