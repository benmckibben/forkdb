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
- **The cause of the gap to WAL was not known when this section was written; it is now: see The gap to WAL below** (file growth, not the number of writes or the sync call). The paragraph that followed here listed hypotheses and is kept as written in the git history.
- **The WSL2 prediction that the ratios between configurations are fsync counts was wrong** and is corrected above. The CPU-bound results held up: the scan is 34 to 49 percent slower than a plain file (21.9 against 15.7 and 16.4 ms), identical on both disks, so it is the layer lookup, pread and checksum, not the disk.
- **The random-read result moved.** On WSL2 forkdb looked faster than the plain file with a rollback journal (3.3 against 8.75 us); here it is 2.96 against 4.11 us, still faster than that configuration and about 30 percent slower than WAL (2.21 us).
- **Fork cost scales with the disk's sync time**, as predicted: one sync, no data copied. On NVMe a fork is about 300 times cheaper than copying this database; on EBS about 22 times.
- **Write amplification and open time are as before** (8.4 KB per commit; the log is 52.1 MB for 34.0 MB live; 18.7 ms to open it, which is the quantity that grows at about 3 s per GiB of log).

Limits of this run: one instance of one type in one availability zone, three repetitions each (the ranges are in the results file and are small), a database of 33.5 MB that fits in memory, no cold-cache reads, one filesystem (ext4, default options). The first attempt at this run lost its output (the console of a terminated instance is empty), so the numbers come from the second run.

## The gap to WAL: it is file growth (2026-10-12)

The last section left the 3x gap to WAL unexplained. A second run on the same kind of host (one `i4i.large`, NVMe instance store and gp3, ext4, 3 repetitions, raw output in [vfs/results/commit-path-real-disks.txt](../vfs/results/commit-path-real-disks.txt)) varied one thing at a time, first on the file alone and then on the store. Nothing in the store, the VFS or the format was changed: the variants wrap the store's I/O in the benchmark.

**Raw patterns of one commit** (two 4,120-byte page records and a 40-byte commit record, then a sync; [vfs/commit_micro.c](../vfs/commit_micro.c)), p50 in microseconds, NVMe and gp3:

| pattern | NVMe | gp3 |
|---|---|---|
| A. append, 3 writes, `fdatasync` (what the store does) | 136.5 | 2,807 |
| B. append, 1 write, `fdatasync` | 134.8 | 2,814 |
| C. file already written to the end (zeros), 3 writes, `fdatasync` | 44.5 | 528 |
| D. already written, 1 write, `fdatasync` | 44.5 | 544 |
| E. `fallocate` first, 1 write, `fdatasync` | 124.2 | 2,722 |
| F. append, 1 write, `O_DSYNC` | 134.7 | 2,806 |
| G. already written, 1 write, `O_DSYNC` | 44.6 | 934 |
| H. already written, 1 write, `fsync` | 44.4 | 526 |

**The store, with its I/O wrapped** (single-row commit p50 in microseconds; the 1000-row column is milliseconds), NVMe and gp3:

| | 1 row, NVMe | 1 row, gp3 | 1000 rows, NVMe | 1000 rows, gp3 | fork, NVMe | fork, gp3 |
|---|---|---|---|---|---|---|
| WAL | 44.5 | 2,327 (range 1,505 to 2,606) | 0.717 | 2.69 | n/a | n/a |
| forkdb as it is | 149.0 | 2,752 | 0.889 | 4.56 | 97.4 | 1,612 |
| forkdb, writes coalesced into one per commit | 147.5 | 2,813 | 0.864 | 4.50 | 97.6 | 1,487 |
| forkdb, log preallocated by writing zeros | 43.3 | 537.5 | 0.729 | 1.56 | 26.4 | 456 |
| both | 42.7 | 530.4 | 0.723 | 1.62 | 26.4 | 492 |

What it shows:

- **The cost is the file growing.** Appending to a file extends its size and allocates blocks, and the `fdatasync` that follows has to commit that metadata through the filesystem journal as well as flush the data. Overwriting blocks that are already allocated and written needs only the data flush: 44 us on NVMe, the same as a WAL commit (44.5 us), where appending is 136 us. A WAL, as I understand SQLite's behavior (I did not trace it), starts over at the beginning of a file that already exists after each checkpoint, so it mostly overwrites; forkdb's log only ever grows.
- **It is not the number of writes** (A against B, C against D, and the coalesced store variant change nothing), **and it is not the choice of sync call** (H, `fsync` on a written file, is as fast as `fdatasync`).
- **`fallocate` does not help** (E, 124 against 135 us): it reserves unwritten extents, and the first write into one converts it, which is a metadata change again. Only blocks that have really been written avoid it.
- **With the log preallocated by writing zeros, a store commit is at WAL parity on NVMe** (43.3 against 44.5 us, a 1000-row commit 0.73 against 0.72 ms) and a fork costs 26 us instead of 97. On gp3 it is 537 us against 2,752 for the store as it is. WAL on gp3 was 992 us in the first run of this series and 2,327 here (with a 1,505 to 2,606 range inside this run), so **the WAL figure on gp3 is not stable between instances and I do not claim a ratio to it there**; the comparison with forkdb is only reliable within one run.

**What these numbers leave out, and it matters.** The zeros are written once, before the timed loop (the first 64 MiB are written when the first table is created), so the commit latencies above are the steady state after preallocation. The zero-fill itself is not free: every byte of log is written twice, once as zeros in a large sequential write and once as data. At about 8.4 KB of log per single-row commit, that is roughly 67 us per commit at the 125 MB/s baseline of gp3 and a few microseconds on NVMe, paid in chunk-sized pauses when the file is extended, or in the background if it were moved off the commit path. A WAL pays the same only in its first lap. **This was not measured there; the next section measures it: all-in, parity holds on gp3 and does not on NVMe.** Also not measured: other filesystems (ext4 only), other chunk sizes, and what the first commit after opening a store costs, because the store truncates a torn tail when it opens and the file would be extended again.

**What it would take to ship** (not done, proposed): extend the file in `store/io_file.c` by writing zeros in chunks (with one `fsync` per chunk) whenever a write would pass the allocated end. The store and the format would not change: recovery already stops at the first invalid record and truncates what follows, and a zero tail is an invalid record. The pieces to decide are the chunk size (the fixed cost per chunk is one sync; the per-byte cost is the second write), whether to keep the preallocation across an open instead of truncating and re-extending, and the disk space (a chunk of zeros past the log end). The existing file and power-loss tests would have to cover it, and a run that includes the extensions (tens of thousands of commits, several chunks) is needed before any claim about the all-in cost.

## Preallocation, measured (2026-10-12)

The log can be preallocated by the layer in [store/io_prealloc.c](../store/io_prealloc.c) (design and tests in [DURABLE-STORE.md](DURABLE-STORE.md)): zeros are written ahead of the end of the log, a chunk at a time, so that a commit overwrites blocks that exist. This section measures it with the zero fill **inside** the timed loop, which the previous section did not: 20,000 single-row commits in a row (a 64 MiB chunk holds about 7,900 of them, so the larger chunks go through two to five extensions and the smaller ones through hundreds), on one `i4i.large` (NVMe instance store and gp3, ext4, 3 repetitions, medians; ranges in [vfs/results/preallocation-sustained.txt](../vfs/results/preallocation-sustained.txt)). The layer sits on top of the counting I/O, so bytes and syncs include the zero fill.

| NVME | WAL | log as it was | 1 MiB chunks | 4 MiB chunks | 16 MiB chunks | 64 MiB chunks |
|---|---|---|---|---|---|---|
| commits per second | 20,134 | 5,459 | 8,898 | 9,408 | 9,447 | 9,921 |
| mean (us) | 49.6 | 183 | 112 | 106 | 106 | 101 |
| p50 (us) | 34.9 | 147 | 43.3 | 43.7 | 43.8 | 43.9 |
| p99 (us) | 147 | 223 | 3,836 | 81.6 | 81.6 | 79.9 |
| p99.9 (us) | 5,614 | 4,821 | 11,622 | 23,057 | 7,625 | 7,574 |
| max (ms) | 5.92 | 4.93 | 11.7 | 30.7 | 84.0 | 290.7 |
| first commit after an open (ms) | 0.49 | 0.33 | 0.46 | 2.90 | 21.0 | 95.2 |
| written per commit (bytes, zero fill included) | n/a | 8,437 | 16,826 | 16,826 | 16,826 | 15,148 |

| EBS | WAL | log as it was | 1 MiB chunks | 4 MiB chunks | 16 MiB chunks | 64 MiB chunks |
|---|---|---|---|---|---|---|
| commits per second | 831 | 345 | 959 | 1,011 | 1,127 | 1,183 |
| mean (us) | 1,203 | 2,895 | 1,043 | 989 | 888 | 845 |
| p50 (us) | 922 | 2,865 | 973 | 953 | 942 | 937 |
| p99 (us) | 2,876 | 3,798 | 1,961 | 1,858 | 1,840 | 1,842 |
| p99.9 (us) | 4,430 | 3,987 | 6,702 | 10,366 | 1,968 | 1,934 |
| max (ms) | 7.68 | 8.63 | 7.86 | 12.3 | 59.1 | 98.9 |
| first commit after an open (ms) | 5.94 | 3.16 | 4.41 | 9.64 | 28.5 | 50.2 |
| written per commit (bytes, zero fill included) | n/a | 8,437 | 16,826 | 16,826 | 16,826 | 15,148 |

What it shows:

- **Against the log as it was, preallocation is a clear win on both disks.** Commits per second: 5,459 to 8,898 to 9,921 on NVMe (1.6 to 1.8 times), 345 to 959 to 1,183 on gp3 (2.8 to 3.4 times). p50 on NVMe: 147 us to 43 us.
- **Against WAL it depends on the disk.** On gp3 forkdb with preallocation is ahead of WAL in this run (959 to 1,183 against 831 commits per second, mean 845 to 1,043 against 1,203 us) and at p50 parity (937 against 922 us). On NVMe it is not: WAL does 20,134 commits per second against 9,921 at best (mean 49.6 against 101 us), and even p50 is 26 percent slower (43.9 against 34.9 us). **The earlier statement that preallocation brings a commit to WAL parity at the steady state holds on gp3 and does not on NVMe.** The WAL figure on gp3 has varied from one instance to the next in this series (992, 2,327 and now 922 us at p50), so treat the gp3 comparison as one run's.
- **The zero fill is not free, and on NVMe it is what is left.** Every commit writes 16,826 bytes instead of 8,437: the data and, amortised, its zeros. The mean is about twice the p50 on NVMe (101 against 44 us): the difference is the extensions, which are stalls, about 4.5 to 11 ms per MiB of chunk on NVMe (a 64 MiB chunk stalls one commit for about 290 ms). On gp3 the stalls are shorter than I expected for the volume's 125 MB/s baseline (about 99 ms for 64 MiB); **I do not know why**, and I did not look.
- **The chunk size trades stall length against frequency, not against total cost.** Small chunks stall rarely-per-byte but often: 1 MiB chunks put the p99 at 3.8 ms on NVMe (one commit in 125 stalls for about 4 ms). Large chunks leave p99 at 80 us and put the cost in the last few in ten thousand: 16 MiB chunks, p99.9 7.6 ms, worst 84 ms; 64 MiB, worst 290 ms. The mean improves a little with chunk size (NVMe 112, 106, 106, 101 us; gp3 1,043, 989, 888, 845 us), because there are fewer syncs for the extensions.
- **A reopen costs the same with or without it** (64 ms for a 169 MB log, about 0.4 s per GiB), **but the first commit after an open pays a whole chunk**: 0.46 ms (1 MiB), 2.9 ms (4 MiB), 21 ms (16 MiB) and 95 ms (64 MiB) on NVMe, against 0.33 ms for the log as it was; gp3: 4.4, 9.6, 28 and 50 ms against 3.2 ms. The second and later commits are as fast as ever. This is what an application sees if it opens a store per request.
- **The file at rest is exactly the log** after a clean close (168,738,268 bytes for every variant), so the trim works.

**Default chunk: 4 MiB** (`FK_PREALLOC_DEFAULT`). It keeps the worst stall near 30 ms on NVMe and 12 ms on gp3 and the first commit after an open near 3 and 10 ms, for a mean within 5 percent (NVMe) and 17 percent (gp3) of the best measured. A latency-insensitive writer would take 16 or 64 MiB, a store opened per request 1 MiB. This is a judgement on three repetitions of one instance type, not an optimum.

**What is still not known or not done:** one instance of one type and 20,000 commits (169 MB), a database that fits in memory, ext4 only; the zero fill runs on the committing thread, so it shows up as stalls (a background fill, or reusing the old log file after a compaction, whose blocks are already written, would avoid both the stalls and the double write; neither is built or measured); disk space is used a chunk ahead of the log; the first commit after an open pays a chunk because recovery truncates the zero tail and the layer starts again from the log's end.

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
