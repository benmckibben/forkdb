# forkdb feasibility spike (throwaway)

One C file, `spike.c`: an **in-memory SQLite VFS** ("layerfs") whose database file is a chain of copy-on-write page layers, with rollback-journal and WAL files kept in memory, plus a test driver. It exists to answer three questions before any product code is written; the answers are in [../docs/SPIKE-FINDINGS.md](../docs/SPIKE-FINDINGS.md).

It is **not** a prototype of the product: nothing is persisted, it is single-process, and the code is written to be read once and discarded.

## Run it

Needs gcc, curl, sha256sum and python3. No make, no system SQLite.

```sh
spike/build.sh batch      # SQLite with the session extension and batch-atomic writes
spike/build/spike-batch   # prints PASS/FAIL per check; exits non-zero on any failure
spike/build.sh stock      # without batch-atomic: the batch-atomic check is then expected to FAIL
```

The first build downloads the pinned SQLite 3.50.0 amalgamation (checksum verified) into `spike/vendor/` and compiles it, which takes several minutes at `-O1` (`OPT=-O0` is faster). Nothing under `vendor/` or `build/` is committed.

## Recorded results

- [results/out-batch-atomic.txt](results/out-batch-atomic.txt): the full final run, `TOTAL FAILS=0`.
- [results/out-stock.txt](results/out-stock.txt): an earlier run on a stock-flag build, before the core-fork microbenchmark was added to `spike.c`. Its one failure is the batch-atomic check, which cannot pass without the compile flag (finding 2 in the findings).

Both were produced on one machine in a single run each; treat the timings as indicative.

## What the checks cover

| Test | Question |
|---|---|
| T1 | Do all rollback-journal modes work on the layered file? |
| T2 | How does `SQLITE_IOCAP_BATCH_ATOMIC` behave: when is it used, what is the size limit, what happens when a commit is refused? |
| T3 | Does WAL work through an in-memory shared-memory layer, and what must a fork carry? |
| T4 | Is a fork O(1), are forks isolated, how does read cost grow with chain depth? |
| T5 | Do session-extension changesets give a usable diff, promote, conflict report and undo, and what do they not capture? |
| T6 | Is every crash point (torn write at I/O step N, then all I/O fails) recoverable to the old or the new state? |
