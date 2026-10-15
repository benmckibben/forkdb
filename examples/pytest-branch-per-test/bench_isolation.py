#!/usr/bin/env python3
"""How much does a fresh database per test cost, by strategy and database size?

A cycle is what a test pays: get a private writable database holding the seed, change ten rows, throw it away.
  forkdb   fork a branch from a snapshot, open it, write, close, discard
  copy     copy the seeded database file, open it, write, close, delete the copy
  backup   sqlite3 backup API from the seeded file into a :memory: database, write, close
  rebuild  create the schema and insert every row again (what a conftest with a seed script does per test)
Usage: bench_isolation.py [rows ...]      (default 1000 20000 200000 1000000)   FORKDB_EXT overrides the library
       WORK=commit|rollback|read   what the test does with its database (default commit): a commit is a durable, fsynced write on a real file
"""
import os, shutil, sqlite3, statistics, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
SO = os.environ.get("FORKDB_EXT") or os.path.normpath(os.path.join(HERE, "..", "..", "build", "ext", "forkdb.so"))
PAYLOAD = "x" * 100


def rows(n):
    return ((i, PAYLOAD, i % 97) for i in range(n))


def schema(c):
    c.execute("create table t(a integer primary key, b text, c integer)")
    c.execute("create index tc on t(c)")


def seed(c, n):
    schema(c)
    c.executemany("insert into t values (?, ?, ?)", rows(n))
    c.commit()


WORK = "commit"  # "commit": a test that writes and commits; "rollback": writes and never commits; "read": only reads


def touch(c):
    if WORK == "read":
        assert c.execute("select count(*) from t where a < 10").fetchone()[0] == 10
        return
    c.execute("update t set b = 'changed' where a < 10")
    if WORK == "commit":
        c.commit()
    assert c.execute("select count(*) from t where a < 10 and b = 'changed'").fetchone()[0] == 10  # a range scan: no table scan in the measurement


def timed(fn, k):
    xs = []
    for _ in range(k):
        t0 = time.perf_counter()
        fn()
        xs.append(time.perf_counter() - t0)
    return xs


def fmt(xs):
    return "%9.2f ms (median)  %9.2f ms (p95)" % (statistics.median(xs) * 1000, sorted(xs)[int(len(xs) * 0.95) - 1] * 1000 if len(xs) > 1 else xs[0] * 1000)


def main():
    global WORK
    WORK = os.environ.get("WORK", "commit")
    sizes = [int(a) for a in sys.argv[1:]] or [1000, 20000, 200000, 1000000]
    tmp = tempfile.mkdtemp(prefix="fk-bench-")
    host = sqlite3.connect(":memory:", check_same_thread=False)
    host.enable_load_extension(True)
    host.load_extension(SO)
    print("sqlite %s, python %s, 100-byte rows plus an index; one cycle = fresh database, 10-row update, discard" % (sqlite3.sqlite_version, sys.version.split()[0]))
    for n in sizes:
        k = 300 if n <= 20000 else (60 if n <= 200000 else 20)
        # --- seed once, as a plain file and as a store
        plain = os.path.join(tmp, "seed-%d.db" % n)
        c = sqlite3.connect(plain)
        t0 = time.perf_counter(); seed(c, n); sec_plain = time.perf_counter() - t0
        c.close()
        fk = os.path.join(tmp, "seed-%d.fk" % n)
        host.execute("select forkdb_init(?, 'main')", (fk,)).fetchall()
        uri = host.execute("select forkdb_open_db(?, 'main')", (fk,)).fetchone()[0]
        c = sqlite3.connect(uri, uri=True)
        t0 = time.perf_counter(); seed(c, n); sec_fk = time.perf_counter() - t0
        c.close()
        host.execute("select forkdb_snapshot(?, 'main', 'seed')", (fk,)).fetchall()
        size_plain = os.path.getsize(plain)
        size_fk0 = os.path.getsize(fk)
        print("\n== %d rows: seed file %.1f MB, seeding %.2fs (plain) / %.2fs (forkdb store, %.1f MB)" % (n, size_plain / 1e6, sec_plain, sec_fk, size_fk0 / 1e6))
        ctr = [0]

        phase = {"fork": [], "open": [], "work": [], "close": [], "discard": []}

        def cyc_fork():
            ctr[0] += 1
            name = "t%d" % ctr[0]
            t = time.perf_counter()
            host.execute("select forkdb_fork(?, 'seed', ?)", (fk, name)).fetchall()
            t1 = time.perf_counter(); phase["fork"].append(t1 - t)
            u = host.execute("select forkdb_open_db(?, ?)", (fk, name)).fetchone()[0]
            d = sqlite3.connect(u, uri=True)
            d.execute("select count(*) from sqlite_master").fetchall()
            t2 = time.perf_counter(); phase["open"].append(t2 - t1)
            touch(d)
            t3 = time.perf_counter(); phase["work"].append(t3 - t2)
            d.close()
            t4 = time.perf_counter(); phase["close"].append(t4 - t3)
            host.execute("select forkdb_discard(?, ?)", (fk, name)).fetchall()
            phase["discard"].append(time.perf_counter() - t4)

        def cyc_copy():
            p = os.path.join(tmp, "copy.db")
            shutil.copyfile(plain, p)
            d = sqlite3.connect(p)
            touch(d)
            d.close()
            os.unlink(p)

        def cyc_backup():
            src = sqlite3.connect(plain)
            d = sqlite3.connect(":memory:")
            src.backup(d)
            src.close()
            touch(d)
            d.close()

        def cyc_rebuild():
            d = sqlite3.connect(":memory:")
            seed(d, n)
            touch(d)
            d.close()

        res = {}
        for name, fn, kk in (("forkdb", cyc_fork, k), ("copy", cyc_copy, k), ("backup", cyc_backup, max(5, k // 4)), ("rebuild", cyc_rebuild, 5 if n >= 200000 else max(10, k // 10))):
            xs = timed(fn, kk)
            res[name] = statistics.median(xs)
            print("  %-8s %s   [%d cycles]" % (name, fmt(xs), kk))
        print("  forkdb phases (median ms): " + ", ".join("%s %.2f" % (p, statistics.median(v) * 1000) for p, v in phase.items()))
        grown = os.path.getsize(fk) - size_fk0
        print("  forkdb store file: %.1f MB after %d fork/write/discard cycles (grew %.2f MB, %.1f KB per cycle)" % (os.path.getsize(fk) / 1e6, ctr[0], grown / 1e6, grown / 1e3 / max(1, ctr[0])))
        print("  speedup of forkdb: %.1fx vs copy, %.1fx vs backup, %.1fx vs rebuild" % (res["copy"] / res["forkdb"], res["backup"] / res["forkdb"], res["rebuild"] / res["forkdb"]))
        host.execute("select forkdb_close(?)", (fk,)).fetchall()
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
