"""End-to-end check of an INSTALLED forkdb Python package (pip install of the wheel), on any platform. It goes beyond test/smoke_py.py:
  1. a durable store asks the operating system to flush (forkdb_sync_count rises) and a nosync store does not;
  2. compaction: the garbage of a discarded branch is reclaimed, the data and the other branches are intact, and a fork made afterwards works;
  3. one store, one process: a second process is refused while a child process has the store open;
  4. a hard kill of that child (no cleanup runs: TerminateProcess on Windows, SIGKILL elsewhere) loses no committed row, leaves the uncommitted one out, and releases the lock.
Run it from outside the repository, with the package installed:  FORKDB_SMOKE_INSTALLED=1 python test/verify_install.py
Exit status 0 only if every check passed."""
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile

import forkdb

failures = []


def check(cond, what, detail=""):
    print(("ok   " if cond else "FAIL ") + what + ("" if cond or not detail else "  [" + detail + "]"))
    sys.stdout.flush()
    if not cond:
        failures.append(what)


print("python", sys.version.split()[0], "| sqlite", sqlite3.sqlite_version, "| platform", sys.platform, "| forkdb", forkdb.__version__)
lib = forkdb.extension_path()
print("library:", lib, "(%d bytes)" % os.path.getsize(lib))
if os.environ.get("FORKDB_SMOKE_INSTALLED") and not os.environ.get("FORKDB_EXTENSION_PATH"):
    check("site-packages" in lib.replace("\\", "/") and "_lib" in lib, "the library is the one bundled in the installed wheel")

host = forkdb.connect()
n = lambda: host.execute("select forkdb_sync_count()").fetchone()[0]
root = tempfile.mkdtemp(prefix="fk-verify-")


def new_store(name, options=None):
    path = os.path.join(root, name)
    if options:
        host.execute("select forkdb_init(?, 'main', ?)", (path, options)).fetchall()
    else:
        host.execute("select forkdb_init(?)", (path,)).fetchall()
    return path


def commits(store, k):
    db = forkdb.open_branch(host, store)
    db.execute("create table t(a integer primary key, b text)")
    for i in range(k):
        db.execute("insert into t values (?, ?)", (i, "x" * 100))
        db.commit()
    db.close()


# 1. durability calls: the number of times the file layer asked the OS to make data durable (FlushFileBuffers on Windows)
before = n()
commits(new_store("durable.fk"), 20)
durable = n() - before
check(durable > 0, "a durable store asks the OS to flush its data (forkdb_sync_count rose by %d for 20 commits)" % durable)
before = n()
commits(new_store("nosync.fk", "nosync"), 20)
nosync = n() - before
check(nosync == 0, "a nosync store never does (forkdb_sync_count rose by %d for 20 commits)" % nosync)

# 2. compaction
store = new_store("compact.fk")
m = forkdb.open_branch(host, store)
m.execute("create table t(a integer primary key, b text)")
m.executemany("insert into t values (?, ?)", [(i, "x" * 100) for i in range(5000)])
m.commit()
m.close()
host.execute("select forkdb_fork(?, 'main', 'junk')", (store,)).fetchall()
j = forkdb.open_branch(host, store, "junk")
j.execute("update t set b = b || 'abcdefghij'")  # rewrites every page on the branch
j.commit()
j.close()
host.execute("select forkdb_discard(?, 'junk')", (store,)).fetchall()
size_before = os.path.getsize(store)
did = host.execute("select forkdb_compact(?)", (store,)).fetchone()[0]
size_after = os.path.getsize(store)  # measured now: the next write extends the file again
check(did == 1, "compaction ran (the discarded branch left the file at least half garbage)", "forkdb_compact returned %r" % (did,))
check(size_after < size_before, "the file got smaller: %d bytes to %d" % (size_before, size_after))
check(host.execute("select forkdb_compact(?)", (store,)).fetchone()[0] == 0, "a second compaction has nothing to do")
m = forkdb.open_branch(host, store)
check(m.execute("select count(*) from t").fetchone()[0] == 5000, "main still has its 5000 rows")
check(m.execute("select count(*) from t where b = ?", ("x" * 100,)).fetchone()[0] == 5000, "and they are unchanged (the junk branch's edits are not in main)")
check(m.execute("pragma integrity_check").fetchone()[0] == "ok", "integrity_check after compaction")
host.execute("select forkdb_fork(?, 'main', 'after')", (store,)).fetchall()
a = forkdb.open_branch(host, store, "after")
a.execute("update t set b = 'A' where a < 100")
a.commit()
check(a.execute("select count(*) from t where b = 'A'").fetchone()[0] == 100 and m.execute("select count(*) from t where b = 'A'").fetchone()[0] == 0,
      "a fork made after the compaction works and is isolated")
a.close()
m.close()

# 3 and 4. a child process holds a store with 50 committed rows and one uncommitted; it is refused to others, then killed hard
CHILD = """
import sys, time
import forkdb
host = forkdb.connect()
store = sys.argv[1]
host.execute("select forkdb_init(?)", (store,)).fetchall()
db = forkdb.open_branch(host, store, isolation_level=None)
db.execute("create table t(a integer primary key, b text)")
db.execute("begin")
for i in range(50):
    db.execute("insert into t values (?, 'x')", (i,))
db.execute("commit")
db.execute("begin")
db.execute("insert into t values (999, 'uncommitted')")
print("READY", flush=True)
time.sleep(300)
"""
kill_store = os.path.join(root, "kill.fk")
child = subprocess.Popen([sys.executable, "-c", CHILD, kill_store], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
try:
    line = child.stdout.readline()
    check(line.strip() == "READY", "a child process opened a store and committed 50 rows", "" if line.strip() == "READY" else "child said %r, stderr: %s" % (line, child.stderr.read()[-300:]))
    if line.strip() == "READY":
        try:
            host.execute("select forkdb_open_db(?, 'main')", (kill_store,)).fetchone()
            check(False, "a second process is refused while the first has the store open")
        except sqlite3.OperationalError as e:
            check("another process" in str(e), "a second process is refused while the first has the store open", str(e))
finally:
    child.kill()  # TerminateProcess on Windows: nothing in the child runs again
    child.wait()
check(child.returncode != 0, "the child was killed, not exited (return code %r)" % (child.returncode,))
try:
    db = forkdb.open_branch(host, kill_store)
    check(True, "the dead process's lock is gone: the store opens")
    check(db.execute("select count(*) from t").fetchone()[0] == 50, "all 50 committed rows survived the kill, and the uncommitted one did not")
    check(db.execute("pragma integrity_check").fetchone()[0] == "ok", "integrity_check after the kill")
    db.execute("insert into t values (1000, 'after')")
    db.commit()
    check(db.execute("select count(*) from t").fetchone()[0] == 51, "and the store takes new writes")
    db.close()
except sqlite3.Error as e:
    check(False, "the store opens after the child was killed", str(e))

host.close()
shutil.rmtree(root, ignore_errors=True)
if failures:
    print("verify_install FAILED: %d check(s)" % len(failures))
    sys.exit(1)
print("verify_install OK")
