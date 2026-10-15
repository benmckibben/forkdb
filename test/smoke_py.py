"""Portable smoke test of the forkdb package, on any platform: loads the extension, makes a branch, writes, and checks that another branch is isolated.
No LD_PRELOAD, no signals. By default it uses python/ in this repository (with FORKDB_EXTENSION_PATH naming the built library); with FORKDB_SMOKE_INSTALLED=1 it
uses the installed wheel and the library bundled in it."""
import os
import sqlite3
import sys
import tempfile

if not os.environ.get("FORKDB_SMOKE_INSTALLED"):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))
import forkdb  # noqa: E402

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


print("python", sys.version.split()[0], "| sqlite", sqlite3.sqlite_version, "| platform", sys.platform, "| forkdb", forkdb.__version__)
print("library:", forkdb.extension_path())
host = forkdb.connect()
info = dict(host.execute("select key, value from forkdb_info").fetchall())
check(info.get("vfs") == "forkdb", "forkdb_info reports the vfs")
check(info.get("version") == forkdb.__version__, "the library's version (%s) is the package's (%s)" % (info.get("version"), forkdb.__version__))

d = tempfile.mkdtemp(prefix="fk-smoke-")
store = os.path.join(d, "s.fk")
host.execute("select forkdb_init(?)", (store,)).fetchall()
main = forkdb.open_branch(host, store)
main.execute("create table t(a integer primary key, b text)")
main.executemany("insert into t values (?, ?)", [(i, "x") for i in range(50)])
main.commit()
host.execute("select forkdb_fork(?, 'main', 'a')", (store,)).fetchall()
a = forkdb.open_branch(host, store, "a")
check(a.execute("select count(*) from t").fetchone()[0] == 50, "a fork starts with its parent's rows")
a.execute("update t set b = 'A' where a < 10")
a.execute("insert into t values (1000, 'new')")
a.commit()
check(a.execute("select count(*) from t where b = 'A'").fetchone()[0] == 10, "the fork sees its own edits")
check(main.execute("select count(*) from t where b = 'A'").fetchone()[0] == 0, "main does not see the fork's edits")
check(main.execute("select count(*) from t").fetchone()[0] == 50, "main still has its 50 rows")
check(a.execute("pragma integrity_check").fetchone()[0] == "ok", "integrity_check on the fork")
host.execute("select forkdb_snapshot(?, 'main', 's')", (store,)).fetchall()
s = forkdb.open_branch(host, store, "s", readonly=True)
check(s.execute("select count(*) from t").fetchone()[0] == 50, "a snapshot reads")
try:
    s.execute("insert into t values (2000, 'z')")
    check(False, "a snapshot refuses writes")
except sqlite3.OperationalError:
    check(True, "a snapshot refuses writes")
for c in (main, a, s):
    c.close()
host.execute("select forkdb_close(?)", (store,)).fetchall()
host.close()
if failures:
    print("smoke_py FAILED: %d check(s)" % len(failures))
    sys.exit(1)
print("smoke_py OK")
