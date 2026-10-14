#!/usr/bin/env python3
"""Tests for the forkdb loadable extension, driven through Python's own sqlite3 module (the host the extension exists for).

Run through scripts/test-ext.sh, which builds build/ext/forkdb.so first. FK_EXT_SO overrides the path of the library.
Standard library only. Nothing here skips: a host whose sqlite3 cannot load extensions fails the run.

The extension keeps a process-global registry of open stores, so every test closes its stores (addCleanup) and every test that can crash the
host runs in a subprocess and asserts on the return code."""
import ctypes
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SO = os.environ.get("FK_EXT_SO") or os.path.join(ROOT, "build", "ext", "forkdb.so")

# Shared by the subprocess scripts: argv[1] is the library, argv[2] a store path, argv[3...] test-specific.
PRELUDE = """
import os, sqlite3, sys
SO, P = sys.argv[1], sys.argv[2]
def load():
    c = sqlite3.connect(':memory:')
    c.enable_load_extension(True)
    c.load_extension(SO)
    return c
"""


def run_py(code, *args):
    return subprocess.run([sys.executable, "-c", PRELUDE + code, SO, *args], capture_output=True, text=True, timeout=120)


class Base(unittest.TestCase):
    def setUp(self):
        self.assertTrue(os.path.exists(SO), "no extension at %s: run scripts/build-ext.sh" % SO)
        self.assertTrue(hasattr(sqlite3.Connection, "enable_load_extension"),
                        "this Python's sqlite3 cannot load extensions: the suite cannot run here")
        self.dir = tempfile.mkdtemp(prefix="fk-ext-")
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.store = os.path.join(self.dir, "s.fk")
        self.conns = []

    def load(self):
        c = sqlite3.connect(":memory:")
        c.enable_load_extension(True)
        c.load_extension(SO)
        self.addCleanup(c.close)
        return c

    def one(self, c, sql, *args):
        return c.execute(sql, args).fetchone()[0]

    def new_store(self, c, root=None, path=None):
        path = path or self.store
        if root is None:
            self.one(c, "select forkdb_init(?)", path)
        else:
            self.one(c, "select forkdb_init(?, ?)", path, root)
        self.addCleanup(self.close_store, c, path)
        return path

    @staticmethod
    def close_store(c, path):
        try:
            c.execute("select forkdb_close(?)", (path,)).fetchone()
        except sqlite3.OperationalError as e:
            if "store not open" not in str(e):
                raise

    def open_branch(self, c, ref="main", path=None):
        uri = self.one(c, "select forkdb_open_db(?, ?)", path or self.store, ref)
        db = sqlite3.connect(uri, uri=True)
        self.addCleanup(db.close)
        return db

    def err(self, c, sql, *args):
        with self.assertRaises(sqlite3.OperationalError) as cm:
            c.execute(sql, args).fetchall()
        return str(cm.exception)


class A_Loading(Base):
    def test_cdll_loads_without_host_symbols(self):
        # The regression the work exists for: before the shim this raised 'undefined symbol: sqlite3_vfs_register'.
        ctypes.CDLL(SO, mode=os.RTLD_NOW | os.RTLD_LOCAL)

    def test_default_entry_point(self):
        c = self.load()
        self.assertEqual(self.one(c, "select count(*) from forkdb_info"), 3)

    def test_explicit_entry_points(self):
        for entry in ("sqlite3_forkdb_init", "sqlite3_extension_init"):
            with self.subTest(entry=entry):
                c = sqlite3.connect(":memory:")
                self.addCleanup(c.close)
                c.enable_load_extension(True)
                c.load_extension(SO, entrypoint=entry)
                self.assertEqual(self.one(c, "select count(*) from forkdb_info"), 3)

    def test_load_twice_on_one_connection(self):
        c = self.load()
        c.load_extension(SO)
        self.assertEqual(self.one(c, "select count(*) from forkdb_info"), 3)

    def test_two_connections_in_one_process(self):
        a, b = self.load(), self.load()
        self.assertEqual(self.one(a, "select count(*) from forkdb_info"), 3)
        self.assertEqual(self.one(b, "select count(*) from forkdb_info"), 3)

    def test_info_exact(self):
        c = self.load()
        self.assertEqual(dict(c.execute("select key, value from forkdb_info").fetchall()),
                         {"session_api": 0, "vfs": "forkdb", "version": "0.1.0"})

    def test_vfs_name_matches_info(self):
        c = self.load()
        self.new_store(c)
        self.assertEqual(self.one(c, "select forkdb_vfs_name(?)", self.store), self.one(c, "select value from forkdb_info where key='vfs'"))

    def test_library_survives_closing_the_loader_connection(self):
        # SQLite dlcloses an extension when the connection that loaded it closes. The library registers a VFS, so it pins itself: closing the
        # loader first, then using a branch connection, once crashed the process (rc -11). Both orders, each in its own process.
        for order in ("loader-first", "branch-first"):
            with self.subTest(order=order):
                r = run_py("""
c = load()
c.execute("select forkdb_init(?)", (P,))
c.execute("select forkdb_close(?)", (P,))
uri = c.execute("select forkdb_open_db(?, 'main')", (P,)).fetchone()[0]
d = sqlite3.connect(uri, uri=True)
d.execute('create table t(a)')
d.commit()
if sys.argv[3] == 'loader-first':
    c.close()
    d.execute('insert into t values (1)')
    d.commit()
    d.close()
else:
    d.close()
    c.close()
print('ok')
""", os.path.join(self.dir, "s.fk"), order)
                self.assertEqual(r.returncode, 0, "rc=%s\n%s%s" % (r.returncode, r.stdout, r.stderr))
                self.assertEqual(r.stdout.strip(), "ok")
                os.unlink(os.path.join(self.dir, "s.fk"))

    def test_missing_library_is_an_error(self):
        c = sqlite3.connect(":memory:")
        self.addCleanup(c.close)
        c.enable_load_extension(True)
        with self.assertRaises(sqlite3.OperationalError):
            c.load_extension(os.path.join(self.dir, "nothing-here.so"))

    def test_no_undefined_sqlite_symbols(self):
        out = subprocess.run(["nm", "-D", "--undefined-only", SO], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual([l for l in out.stdout.splitlines() if "sqlite3" in l], [])


class B_Functions(Base):
    def test_init_creates_file_and_second_init_is_refused(self):
        c = self.load()
        self.assertIsNone(self.one(c, "select forkdb_init(?)", self.store))
        self.addCleanup(self.close_store, c, self.store)
        self.assertTrue(os.path.getsize(self.store) > 0)
        self.assertIn("store is already open", self.err(c, "select forkdb_init(?)", self.store))

    def test_custom_root_name(self):
        c = self.load()
        self.new_store(c, root="trunk")
        self.open_branch(c, "trunk")
        self.assertIn("no branch named 'main'", self.err(c, "select forkdb_open_db(?, 'main')", self.store))

    def test_fork_and_snapshot_return_distinct_nonzero_uids(self):
        c = self.load()
        self.new_store(c)
        a = self.one(c, "select forkdb_fork(?, 'main', 'a')", self.store)
        b = self.one(c, "select forkdb_fork(?, 'main', 'b')", self.store)
        s = self.one(c, "select forkdb_snapshot(?, 'main', 's1')", self.store)
        self.assertEqual(len({a, b, s}), 3)
        for v in (a, b, s):
            self.assertIsInstance(v, int)
            self.assertNotEqual(v, 0)

    def test_fork_errors(self):
        c = self.load()
        self.new_store(c)
        self.one(c, "select forkdb_fork(?, 'main', 'a')", self.store)
        self.assertIn("a branch named 'a' exists", self.err(c, "select forkdb_fork(?, 'main', 'a')", self.store))
        self.assertIn("no branch named 'nope'", self.err(c, "select forkdb_fork(?, 'nope', 'b')", self.store))

    def test_open_db_uri_shape_and_unknown_ref(self):
        c = self.load()
        self.new_store(c)
        uri = self.one(c, "select forkdb_open_db(?, 'main')", self.store)
        self.assertRegex(uri, r"^file:b[0-9]+\?vfs=forkdb$")
        self.assertIn("no branch named 'nope'", self.err(c, "select forkdb_open_db(?, 'nope')", self.store))

    def test_discard(self):
        c = self.load()
        self.new_store(c)
        self.one(c, "select forkdb_fork(?, 'main', 'a')", self.store)
        self.assertIsNone(self.one(c, "select forkdb_discard(?, 'a')", self.store))
        self.assertIn("no branch named 'a'", self.err(c, "select forkdb_open_db(?, 'a')", self.store))
        self.assertIn("no branch named 'zzz'", self.err(c, "select forkdb_discard(?, 'zzz')", self.store))
        self.assertIn("the root branch cannot be discarded", self.err(c, "select forkdb_discard(?, 'main')", self.store))

    def test_discard_and_reset_refused_while_a_connection_is_open(self):
        c = self.load()
        self.new_store(c)
        self.one(c, "select forkdb_fork(?, 'main', 'a')", self.store)
        db = self.open_branch(c, "a")
        db.execute("create table x(a)")
        db.commit()
        self.assertIn("1 connection(s) are open on 'a'", self.err(c, "select forkdb_discard(?, 'a')", self.store))
        self.assertIn("1 connection(s) are open on 'a'", self.err(c, "select forkdb_reset(?, 'a', 'main')", self.store))
        db.close()
        self.assertIsNone(self.one(c, "select forkdb_discard(?, 'a')", self.store))

    def test_compact_returns_zero_below_threshold_and_keeps_data(self):
        # A small store is under the compaction threshold, so this is "does not run, does not harm", not a compaction test.
        c = self.load()
        self.new_store(c)
        db = self.open_branch(c)
        db.execute("create table t(a integer primary key)")
        db.executemany("insert into t values (?)", [(i,) for i in range(100)])
        db.commit()
        db.close()
        self.assertEqual(self.one(c, "select forkdb_compact(?)", self.store), 0)
        db = self.open_branch(c)
        self.assertEqual(self.one(db, "select count(*) from t"), 100)

    def test_close_then_reopen(self):
        c = self.load()
        self.new_store(c)
        db = self.open_branch(c)
        db.execute("create table t(a)")
        db.execute("insert into t values (7)")
        db.commit()
        db.close()
        self.assertIsNone(self.one(c, "select forkdb_close(?)", self.store))
        self.assertIn("store not open", self.err(c, "select forkdb_close(?)", self.store))
        db = self.open_branch(c)  # reopened on first use
        self.assertEqual(self.one(db, "select a from t"), 7)

    def test_session_functions_report_no_session_api(self):
        c = self.load()
        self.new_store(c)
        for sql in ("select forkdb_diff(?, 'main')", "select forkdb_promote(?, 'main')", "select forkdb_undo(?, 'main')"):
            with self.subTest(sql=sql):
                self.assertIn("no session API", self.err(c, sql, self.store))

    def test_null_and_missing_arguments(self):
        c = self.load()
        self.new_store(c)
        cases = [
            ("select forkdb_init(NULL)", "init: path is NULL", ()),
            ("select forkdb_init()", "init: need a path", ()),
            ("select forkdb_fork(?, NULL, NULL)", "fork: need from and name", (self.store,)),
            ("select forkdb_snapshot(?)", "snapshot: need from and name", (self.store,)),
            ("select forkdb_discard(?)", "discard: need a name", (self.store,)),
            ("select forkdb_reset(?)", "reset: need a name", (self.store,)),
            ("select forkdb_open_db(NULL)", "store path is NULL", ()),
            ("select forkdb_vfs_name(NULL)", "store path is NULL", ()),
            ("select forkdb_vfs_name()", "need a store path", ()),
            ("select forkdb_close(NULL)", "close: path is NULL", ()),
        ]
        for sql, msg, args in cases:
            with self.subTest(sql=sql):
                self.assertIn(msg, self.err(c, sql, *args))

    def test_functions_are_direct_only(self):
        c = self.load()
        c.execute("create view v as select forkdb_vfs_name('x')")
        self.assertIn("unsafe use of forkdb_vfs_name()", self.err(c, "select * from v"))
        c.execute("create table ta(a)")
        c.execute("create trigger tr after insert on ta begin select forkdb_fork('x', 'main', 'y'); end")
        self.assertIn("unsafe use of forkdb_fork()", self.err(c, "insert into ta values (1)"))


class C_OpenFailureDoesNotCrash(Base):
    """A store that cannot be opened must raise an SQL error. It once killed the host (fkx_errmsg(NULL)); each case runs in its own process so
    a regression is one failed test with a return code, not a dead test runner."""

    SCRIPT = """
c = load()
sql = sys.argv[3]
try:
    c.execute(sql, (P,)).fetchall()
except sqlite3.OperationalError as e:
    print('error:', e)
else:
    print('NO ERROR')
    sys.exit(3)
print('alive:', c.execute('select 1').fetchone()[0])
"""
    FUNCS = [
        ("select forkdb_vfs_name(?)", ()),
        ("select forkdb_fork(?, 'main', 'x')", ()),
        ("select forkdb_snapshot(?, 'main', 'x')", ()),
        ("select forkdb_open_db(?, 'main')", ()),
        ("select forkdb_compact(?)", ()),
        ("select forkdb_discard(?, 'x')", ()),
        ("select forkdb_reset(?, 'x', 'main')", ()),
        ("select forkdb_diff(?, 'main')", ()),
    ]

    def check(self, path, sql):
        r = run_py(self.SCRIPT, path, sql)
        self.assertEqual(r.returncode, 0, "%s on %s: rc=%s\n%s%s" % (sql, path, r.returncode, r.stdout, r.stderr))
        self.assertIn("alive: 1", r.stdout)
        return r.stdout

    def test_missing_store(self):
        path = os.path.join(self.dir, "missing.fk")
        for sql, _ in self.FUNCS:
            with self.subTest(sql=sql):
                self.check(path, sql)
        self.assertFalse(os.path.exists(path), "a failed open must not create the file")

    def test_missing_store_message(self):
        out = self.check(os.path.join(self.dir, "missing.fk"), "select forkdb_vfs_name(?)")
        self.assertIn("no store at", out)

    def test_init_in_a_directory_that_does_not_exist(self):
        out = self.check(os.path.join(self.dir, "no", "such", "dir", "x.fk"), "select forkdb_init(?)")
        self.assertIn("cannot open the store file", out)

    def test_file_that_is_not_a_store(self):
        path = os.path.join(self.dir, "junk.fk")
        with open(path, "wb") as f:
            f.write(b"x" * 5000)
        for sql, _ in self.FUNCS:
            with self.subTest(sql=sql):
                self.check(path, sql)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), b"x" * 5000, "a refused open must leave the file alone")


class D_EndToEnd(Base):
    def make_table(self, db, n):
        db.execute("create table t(a integer primary key, b text)")
        db.execute("create index tb on t(b)")
        db.executemany("insert into t values (?, ?)", [(i, "v%d" % i) for i in range(n)])
        db.commit()

    def test_fork_isolation_and_integrity(self):
        c = self.load()
        self.new_store(c)
        main = self.open_branch(c)
        self.make_table(main, 5000)
        main.close()
        self.one(c, "select forkdb_fork(?, 'main', 'feat')", self.store)
        feat = self.open_branch(c, "feat")
        feat.execute("update t set b = 'changed' where a < 100")
        feat.execute("delete from t where a >= 100 and a < 200")
        feat.executemany("insert into t values (?, ?)", [(10000 + i, "new") for i in range(100)])
        feat.commit()
        main = self.open_branch(c)
        self.assertEqual(main.execute("select count(*), sum(a) from t").fetchone(), (5000, sum(range(5000))))
        self.assertEqual(main.execute("select count(*) from t where b = 'changed'").fetchone()[0], 0)
        self.assertEqual(feat.execute("select count(*), sum(a) from t").fetchone(),
                         (5000, sum(range(5000)) - sum(range(100, 200)) + sum(10000 + i for i in range(100))))
        self.assertEqual(feat.execute("select count(*) from t where b = 'changed'").fetchone()[0], 100)
        for db in (main, feat):
            self.assertEqual(db.execute("pragma integrity_check").fetchall(), [("ok",)])

    def test_uncommitted_work_is_not_persisted(self):
        c = self.load()
        self.new_store(c)
        db = self.open_branch(c)
        db.execute("create table t(a)")
        db.execute("insert into t values (1)")
        db.commit()
        db.execute("insert into t values (2)")
        db.close()  # sqlite3 rolls the open transaction back
        db = self.open_branch(c)
        self.assertEqual(db.execute("select a from t").fetchall(), [(1,)])

    def test_reset_makes_a_branch_read_its_target(self):
        c = self.load()
        self.new_store(c)
        main = self.open_branch(c)
        self.make_table(main, 50)
        main.close()
        self.one(c, "select forkdb_fork(?, 'main', 'feat')", self.store)
        feat = self.open_branch(c, "feat")
        feat.execute("delete from t")
        feat.execute("create table extra(x)")
        feat.commit()
        feat.close()
        self.assertIsNone(self.one(c, "select forkdb_reset(?, 'feat', 'main')", self.store))
        feat = self.open_branch(c, "feat")
        self.assertEqual(feat.execute("select count(*) from t").fetchone()[0], 50)
        self.assertEqual(feat.execute("select count(*) from sqlite_master where name = 'extra'").fetchone()[0], 0)
        self.assertEqual(feat.execute("pragma integrity_check").fetchall(), [("ok",)])

    def test_snapshot_is_frozen(self):
        c = self.load()
        self.new_store(c)
        main = self.open_branch(c)
        self.make_table(main, 20)
        self.one(c, "select forkdb_snapshot(?, 'main', 's1')", self.store)
        main.execute("insert into t values (100, 'later')")
        main.commit()
        snap = sqlite3.connect(self.one(c, "select forkdb_open_db(?, 's1')", self.store).replace("?vfs", "?mode=ro&vfs"), uri=True)
        self.addCleanup(snap.close)
        self.assertEqual(snap.execute("select count(*) from t").fetchone()[0], 20)
        with self.assertRaises(sqlite3.OperationalError):
            snap.execute("insert into t values (101, 'no')")

    def test_data_survives_close_and_a_new_process(self):
        c = self.load()
        path = self.new_store(c)
        main = self.open_branch(c)
        self.make_table(main, 300)
        main.close()
        self.one(c, "select forkdb_close(?)", path)
        r = run_py("""
c = load()
uri = c.execute("select forkdb_open_db(?, 'main')", (P,)).fetchone()[0]
d = sqlite3.connect(uri, uri=True)
print(d.execute('select count(*) from t').fetchone()[0], d.execute('pragma integrity_check').fetchone()[0])
""", path)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.split(), ["300", "ok"])

    DEATH = """
c = load()
uri = c.execute("select forkdb_open_db(?, 'main')", (P,)).fetchone()[0]
d = sqlite3.connect(uri, uri=True, isolation_level=None)
d.execute("create table t(a integer primary key)")
d.execute("begin")
for i in range(10):
    d.execute("insert into t values (?)", (i,))
d.execute("commit")
if sys.argv[3] == 'open-transaction':
    d.execute("begin")
    for i in range(10, 20):
        d.execute("insert into t values (?)", (i,))
os._exit(9)
"""

    def death(self, mode):
        c = self.load()
        path = self.new_store(c)
        self.one(c, "select forkdb_close(?)", path)  # the child is the only process with the store open
        r = run_py(self.DEATH, path, mode)
        self.assertEqual(r.returncode, 9, r.stdout + r.stderr)
        db = self.open_branch(c, path=path)
        self.assertEqual(db.execute("select count(*), sum(a) from t").fetchone(), (10, 45))
        self.assertEqual(db.execute("pragma integrity_check").fetchall(), [("ok",)])

    def test_process_dies_after_commit(self):
        self.death("committed")

    def test_process_dies_with_a_transaction_open(self):
        self.death("open-transaction")


class E_OneProcessAtATime(Base):
    """The store file is exclusively locked; a second process is refused, the lock is released on close
    or crash, and multiple racing openers see exactly one winner at a time."""

    def test_second_process_is_refused(self):
        c = self.load()
        self.new_store(c)
        db = self.open_branch(c)
        db.execute("create table t(a)")
        db.executemany("insert into t values (?)", [(i,) for i in range(3)])
        db.commit()
        # Keep the store OPEN in this process (do not forkdb_close).
        # Run a subprocess that tries to open it; it must fail with 'open in another process'.
        r = run_py("""
c = load()
try:
    c.execute("select forkdb_open_db(?, 'main')", (P,)).fetchone()
except sqlite3.OperationalError as e:
    print('refused:', e)
    sys.exit(0)
print('NOT REFUSED')
sys.exit(3)
""", self.store)
        self.assertEqual(r.returncode, 0, "rc=%s\n%s%s" % (r.returncode, r.stdout, r.stderr))
        self.assertIn("open in another process", r.stdout)
        # The parent process still reads its 3 rows.
        self.assertEqual(db.execute("select count(*) from t").fetchone()[0], 3)
        self.assertEqual(db.execute("pragma integrity_check").fetchall(), [("ok",)])

    def test_second_open_in_the_same_process_is_allowed_by_registry(self):
        # The extension keeps a process-global registry: loading the extension on a second
        # connection and calling forkdb_open_db reuses the already-open store rather than
        # re-opening the file.  The file lock therefore never conflicts.  A subprocess is
        # needed to see the refusal, as tested above.
        c = self.load()
        self.new_store(c)
        db = self.open_branch(c)
        db.execute("create table t(a integer primary key)")
        db.execute("insert into t values (1)")
        db.commit()
        c2 = self.load()
        uri = self.one(c2, "select forkdb_open_db(?, 'main')", self.store)
        db2 = sqlite3.connect(uri, uri=True)
        self.addCleanup(db2.close)
        self.assertEqual(db2.execute("select a from t").fetchone(), (1,))
        # New-style cleanup after successful open (the task description mentions this pattern).
        self.addCleanup(self.close_store, c2, self.store)

    def test_lock_is_released_when_the_holder_dies(self):
        # Forkdb_close the parent's store first so the subprocess is the only holder.
        c = self.load()
        self.new_store(c)
        self.one(c, "select forkdb_close(?)", self.store)
        path = self.store
        r = run_py("""
c = load()
c.execute("select forkdb_init(?)", (P,)).fetchone()
os._exit(9)
""", path)
        self.assertEqual(r.returncode, 9, r.stdout + r.stderr)
        # Now the parent can open the store again.
        uri = self.one(c, "select forkdb_open_db(?, 'main')", path)
        self.addCleanup(self.close_store, c, path)
        db = sqlite3.connect(uri, uri=True)
        self.addCleanup(db.close)
        db.execute("create table x(b)")
        db.execute("insert into x values (42)")
        db.commit()
        self.assertEqual(db.execute("select b from x").fetchone()[0], 42)

    def test_lock_is_released_by_forkdb_close(self):
        c = self.load()
        path = self.new_store(c)
        self.one(c, "select forkdb_close(?)", path)
        r = run_py("""
c = load()
c.execute("select forkdb_open_db(?, 'main')", (P,)).fetchone()
print('ok')
""", path)
        self.assertEqual(r.returncode, 0, "rc=%s\n%s%s" % (r.returncode, r.stdout, r.stderr))
        self.assertIn("ok", r.stdout)

    def test_two_processes_racing(self):
        import time
        c = self.load()
        path = self.new_store(c)
        db = self.open_branch(c, path=path)
        db.execute("create table t(a integer primary key)")
        db.executemany("insert into t values (?)", [(i,) for i in range(10)])
        db.commit()
        db.close()
        self.one(c, "select forkdb_close(?)", path)
        # The store is now closed; 6 subprocesses race to open it.
        RACE = """
import time, sys
try:
    c = load()
    c.execute("select forkdb_open_db(?, 'main')", (P,)).fetchone()
    time.sleep(2)
    print('held')
except sqlite3.OperationalError as e:
    print('refused:', e)
"""
        t0 = time.monotonic()
        procs = [subprocess.Popen([sys.executable, "-c", PRELUDE + RACE, SO, path],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                 for _ in range(6)]
        outputs = []
        for p in procs:
            out, err = p.communicate(timeout=10)
            self.assertEqual(p.returncode, 0, "rc=%s out=%s err=%s" % (p.returncode, out, err))
            outputs.append(out.strip())
        dt = time.monotonic() - t0
        self.assertLess(dt, 10, "race test took %.1fs (limit 10s)" % dt)
        held = [o for o in outputs if o == "held"]
        refused = [o for o in outputs if o.startswith("refused:")]
        self.assertEqual(len(held) + len(refused), 6, "every process either held the store or was refused: %r" % outputs)
        self.assertGreaterEqual(len(held), 1)
        self.assertGreaterEqual(len(refused), 1)
        for o in refused:
            self.assertIn("open in another process", o)
        # The store must be usable afterwards.
        uri = self.one(c, "select forkdb_open_db(?, 'main')", path)
        self.addCleanup(self.close_store, c, path)
        db = sqlite3.connect(uri, uri=True)
        self.addCleanup(db.close)
        self.assertEqual(db.execute("select count(*) from t").fetchone()[0], 10)
        self.assertEqual(db.execute("pragma integrity_check").fetchall(), [("ok",)])


class F_Threads(Base):
    """Connections on separate branches used from separate threads, and branch management from another thread, through Python's sqlite3 (which
    releases the GIL while SQLite runs, so the threads really overlap in the VFS). Without the VFS mutex this failed with 'disk I/O error' on
    concurrent commits to different branches. Each case runs in its own process and asserts on the return code: a crash is one failed test."""

    SCRIPT = """
import random, threading, time
mode = sys.argv[3]
host = sqlite3.connect(':memory:', check_same_thread=False)
host.enable_load_extension(True)
host.load_extension(SO)
host.execute("select forkdb_init(?)", (P,)).fetchall()
def conn(ref):
    uri = host.execute("select forkdb_open_db(?, ?)", (P, ref)).fetchone()[0]
    return sqlite3.connect(uri, uri=True, check_same_thread=False, timeout=5, isolation_level=None)
m = conn('main')
m.execute("create table t(a integer primary key, b)")
m.execute("begin")
for i in range(200):
    m.execute("insert into t values (?, 'x')", (i,))
m.execute("commit")
m.close()
for n in ('a', 'b', 'c'):
    host.execute("select forkdb_fork(?, 'main', ?)", (P, n)).fetchall()
refs = ['a', 'a', 'a'] if mode == 'same-branch' else ['main', 'a', 'b', 'c']
conns = [conn(r) for r in refs]
ok = [0] * len(conns)
errs = []
ITERS = 120
def writer(i):
    c = conns[i]
    base = 100000 * (i + 1)
    for k in range(ITERS):
        for attempt in range(400):
            try:
                c.execute("begin immediate")
                c.execute("insert into t values (?, 'w')", (base + k,))
                c.execute("commit")
                ok[i] += 1
                break
            except sqlite3.OperationalError as e:
                try:
                    c.execute("rollback")
                except sqlite3.Error:
                    pass
                if 'locked' in str(e) or 'busy' in str(e):
                    time.sleep(random.random() / 500)
                    continue
                errs.append('writer %d: %s' % (i, e))
                return
def manager():
    for k in range(20):
        try:
            name = 'f%d' % k
            host.execute("select forkdb_fork(?, 'main', ?)", (P, name)).fetchall()
            host.execute("select forkdb_discard(?, ?)", (P, name)).fetchall()
        except Exception as e:
            errs.append('manager: %s: %s' % (type(e).__name__, e))
        time.sleep(0.002)
ts = [threading.Thread(target=writer, args=(i,)) for i in range(len(conns))]
if mode == 'with-manager':
    ts.append(threading.Thread(target=manager))
for t in ts:
    t.start()
for t in ts:
    t.join()
bad = list(errs)
total = 0
for i, c in enumerate(conns):
    n = c.execute("select count(*) from t where b = 'w'").fetchone()[0]
    total += n if mode == 'same-branch' and i == 0 else 0
    if mode != 'same-branch' and n != ok[i]:
        bad.append('connection %d: %d rows, %d commits acknowledged' % (i, n, ok[i]))
    if c.execute("pragma integrity_check").fetchall() != [('ok',)]:
        bad.append('integrity_check on connection %d' % i)
if mode == 'same-branch' and total != sum(ok):
    bad.append('same branch: %d rows, %d commits acknowledged' % (total, sum(ok)))
print('commits', ok, 'problems', bad[:4])
sys.exit(1 if bad else 0)
"""

    REGISTRY = """
import threading
errs = []
host = load()
host.execute("select forkdb_init(?)", (P,)).fetchall()
host.execute("select forkdb_close(?)", (P,)).fetchall()
def worker(i):
    c = load()  # one connection per thread: a shared connection would serialise the calls by itself
    for k in range(150):
        try:
            if (k + i) % 3 == 0:
                c.execute("select forkdb_close(?)", (P,)).fetchall()
            else:
                c.execute("select forkdb_vfs_name(?)", (P,)).fetchall()
        except sqlite3.OperationalError as e:
            if 'store not open' not in str(e):
                errs.append('%d: %s' % (i, e))
ts = [threading.Thread(target=worker, args=(i,)) for i in range(6)]
for t in ts:
    t.start()
for t in ts:
    t.join()
print('problems', errs[:4])
sys.exit(1 if errs else 0)
"""

    def test_registry_is_safe_across_connections_and_threads(self):
        # forkdb_close frees the entry another thread may be using: the registry lock must make each call whole.
        path = os.path.join(self.dir, "s.fk")
        r = run_py(self.REGISTRY, path)
        self.assertEqual(r.returncode, 0, "rc=%s\\n%s%s" % (r.returncode, r.stdout, r.stderr))
        self.assertIn("problems []", r.stdout)

    def run_mode(self, mode):
        r = run_py(self.SCRIPT, os.path.join(self.dir, "s.fk"), mode)
        self.assertEqual(r.returncode, 0, "%s: rc=%s\n%s%s" % (mode, r.returncode, r.stdout, r.stderr))
        self.assertIn("problems []", r.stdout)

    def test_writers_on_separate_branches(self):
        self.run_mode("separate")

    def test_writers_on_the_same_branch(self):
        self.run_mode("same-branch")

    def test_branch_management_while_writers_run(self):
        self.run_mode("with-manager")


if __name__ == "__main__":
    unittest.main()
