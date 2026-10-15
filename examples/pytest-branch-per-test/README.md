# A fresh database for every pytest test, from a branch

forkdb gives each test its own writable copy of a seeded SQLite database by **forking a branch** from a snapshot instead of copying a file or rebuilding the
schema. This directory is the proof that it works on a real test suite, and the numbers on what it costs and when it pays.

```python
# conftest.py
pytest_plugins = ["forkdb_pytest"]

@pytest.fixture(scope="session")
def forkdb_seed():
    return seed_database            # callable(conn): create the schema, insert the rows every test starts from

# test_orders.py
def test_cancel_restores_stock(db):                 # db: an open sqlite3.Connection on this test's own branch
    ...
def test_app(db_uri):                               # db_uri: for code that opens its own connection
    shop = Shop(db_uri)
```

Per process, once: create a store, run the seed, freeze it with a snapshot. Per test: `forkdb_fork` from the snapshot, open it, run the test, close, `forkdb_discard`.
Whatever the test writes is gone afterwards, pass or fail. Under pytest-xdist each worker makes its own store (a store file is held by one process at a time).

## What is here

| | |
|---|---|
| `forkdb_pytest.py` | the plugin: fixtures `db`, `db_uri`, `forkdb_seed`, `forkdb_configure`, `forkdb_stats`, options `--forkdb-dir`, `--forkdb-max-mb`, `--forkdb-durable` |
| `shop/`, `tests/` | a small order system (customers, products, orders, a view, a trigger, check constraints) and **116 tests** for it, written against the fixtures only. Several tests wreck their database on purpose |
| `canary/` | isolation canaries: 283 tests that delete, drop, overwrite and leave transactions open, and others that check the seed is intact; run in order, reversed, shuffled, under 4 workers, on tmpfs, after a failing test, and with a tiny size limit |
| `scale/` | the same 200 tests under forkdb, a file copy per test and a reseed per test, at 20 thousand to 1 million rows |
| `bench_isolation.py` | the cost of one fresh database by strategy, with the phases of a fork |

Run everything: `scripts/test-pytest-example.sh` (builds the extension, installs pytest and pytest-xdist into `build/pylib`, runs the suite serially, with 4 workers, on tmpfs, then the canaries). It is a CI job.
Needs Python whose `sqlite3` can load extensions and `build/ext/forkdb.so` (`scripts/build-ext.sh`, or `FORKDB_EXT`).

## What it costs, measured

Linux, ext4 on WSL2's virtual disk versus tmpfs (`/dev/shm`), Python 3.12, SQLite 3.45. One "cycle" is what a test pays: get a private database that holds the seed, update ten rows,
commit, throw it away. Median of 20 to 300 cycles (`bench_isolation.py`; `NOSYNC=1` opens the store without fsync).

| seed rows (file size) | forkdb, fsync | **forkdb, nosync** | file copy | sqlite backup to memory | reseed | forkdb with fsync, tmpfs | file copy, tmpfs |
|---|---|---|---|---|---|---|---|
| 1 000 (0.1 MB) | 8.5 ms | **0.09 ms** | 9.6 ms | 0.20 ms | 0.9 ms | 0.09 ms | 0.29 ms |
| 20 000 (2.5 MB) | 8.7 ms | **0.10 ms** | 12.1 ms | 0.81 ms | 18.8 ms | 0.10 ms | 1.3 ms |
| 200 000 (25 MB) | 8.8 ms | **0.09 ms** | 32.7 ms | 7.4 ms | 216 ms | 0.08 ms | 11.3 ms |
| 1 000 000 (125 MB) | 8.3 ms | **0.09 ms** | 123.5 ms | 51.3 ms | 1 155 ms | 0.09 ms | 58.1 ms |

* **The cost of a fork does not depend on the size of the database.** It copies no pages.
* **With fsync, a fork and a discard each end in an fsync** (about 3.8 ms apiece): that is the 8.5 ms floor, and on a disk it makes a file copy, a backup to memory or a reseed as fast or faster below a few hundred thousand rows.
* **Without fsync the floor is gone**: 0.01 ms for a fork and 0.01 ms for a discard, the whole cycle 0.09 ms at every size, faster than every other strategy in the table. The plugin therefore opens its stores with `nosync` by default
  (`--forkdb-durable` puts the fsyncs back). That is safe here because the store is throwaway: it is deleted at the end of the session, and the data survives the death of the test process. Only a power cut loses it
  (see [DURABLE-STORE.md](../../docs/DURABLE-STORE.md), "Running without syncs"). On tmpfs fsync is already almost free, so there the two modes are the same.

The same 200 tests (100 reads, 60 committed writes, 40 uncommitted writes), whole pytest process in seconds (`scale/run_scale.sh`):

| seed rows | **forkdb, disk** | forkdb with fsync, disk | copy, disk | reseed, disk | forkdb, tmpfs | copy, tmpfs | reseed, tmpfs |
|---|---|---|---|---|---|---|---|
| 20 000 | **0.48** | 2.03 | 1.50 | 12.7 | 0.46 | 0.74 | 2.5 |
| 200 000 | **0.80** | 2.39 | 5.74 | 61.7 | 0.76 | 3.55 | 49.0 |
| 1 000 000 | **2.82** | 4.61 | 26.2 | n/a | 2.82 | 16.4 | n/a |

(Disk columns from one run of the final script; the tmpfs columns from the earlier idle-machine run. An earlier version of the script timed with the wall clock, which jumped during one run, so it now uses `/proc/uptime`; pytest's own reported times agreed with the numbers kept here.)

The shop suite itself (116 tests, a seed of about 1 MB), on a disk: forkdb with fsync 1.43 s, **forkdb with the plugin's default (nosync) 0.53 s**, file copy 0.96 s, reseed about 15 s (113 tests when the copy and reseed were measured, 1.34 s and 0.96 s then).
With fsync, a small seed on a disk was a case where copying the file was as good; without it forkdb is the fastest strategy at every size measured.

## It grows, and the plugin compacts it

Discarding a branch does not give its pages back. Measured over 4 000 tests on a 20 000-row seed: tests that commit a small change grew the store from 8.4 MB to 126 MB (about 31 KB per test),
tests that rewrite 5 000 rows grew it to 2.3 GB (about 570 KB per test), read-only tests did not grow it at all. `forkdb_compact` gave all of it back (2.3 MB, same rows, `integrity_check` ok).
The plugin runs it whenever the file passes `--forkdb-max-mb` (default 64): 600 tests that each rewrote 5 000 rows kept the file at 42 MB, with 5 compactions that took 0.12 s in all.

## What the proof does and does not show

Shown, by tests that fail when the property breaks:
* **Isolation.** The canaries delete every row, drop tables, rewrite everything, add 20 000 rows, leave a transaction open, create tables; the tests after them still see exactly the seed,
  in order, reversed, shuffled, under 4 workers, after a failed test, and across compactions. Four plugin mutants were tried: a branch that is never discarded (it survived until the leak check asked the store whether the old branches still exist instead of trusting the plugin's own counters, then it failed 5 of the 8 canary modes),
  tests connected to the shared root branch (fails), forking from the root instead of the snapshot (survives: equivalent, nothing writes to the root) and reusing one branch name (survives: equivalent, a discarded name is free).
* **A real application suite.** 116 tests of an order system, including rollback on partial failure, constraints, a trigger and a view, pass serially, under 4 workers and on tmpfs. Ten application mutants were tried
  (no stock check, no rollback, no refund, a cancelled order cancelled again, revenue counting cancelled orders, a raw `IntegrityError`, no restock, `top_customers` counting cancelled orders, no commit,
  duplicate lines not merged): each makes at least one test fail.

Not shown, and limits:
* **One process per store file.** The file is locked while open. Under xdist every worker gets its own store (and pays the seed once each); two pytest runs at the same time are fine, each makes its own directory under `--forkdb-dir`.
* **Python's `sqlite3` must be able to load extensions** (some distributions and macOS system Pythons cannot). The plugin refuses with a message when it cannot.
* **A branch is addressed by a store id that is only stable until the next fork, discard or reset**: the plugin opens the URI at once; code that keeps a `db_uri` string and reconnects later, after other tests ran in the same process, must not.
* **The portable extension has no diff, promote or undo.** This is about isolation, not about reviewing what a test changed.
* **Linux only so far**; macOS and Windows have not been run. The store is a plain file under a temporary directory and is removed at the end of the session.
