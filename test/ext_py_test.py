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
        self.assertRegex(uri, r"^file:/forkdb/b[0-9]+\?vfs=forkdb$")
        self.assertIn("no branch named 'nope'", self.err(c, "select forkdb_open_db(?, 'nope')", self.store))

    def test_new_ids_fit_in_a_javascript_number(self):
        # forkdb_fork and forkdb_snapshot return the new branch's uid. node:sqlite throws on an integer above 2^53 (after the fork has happened), so every id must
        # fit. 64 random bits would be above 2^53 for all but 1 in 1024 of them: 100 draws make a failure certain.
        c = self.load()
        self.new_store(c)
        ids = []
        for i in range(50):
            ids.append(self.one(c, "select forkdb_fork(?, 'main', ?)", self.store, "f%d" % i))
            ids.append(self.one(c, "select forkdb_snapshot(?, 'main', ?)", self.store, "s%d" % i))
        self.assertEqual(len(set(ids)), 100, "ids are distinct")
        for u in ids:
            self.assertTrue(0 < u < 2 ** 53, "uid %d does not fit in a JavaScript number" % u)

    def test_uri_read_as_a_plain_filename_fails_loudly(self):
        # A driver built with URI filenames off (better-sqlite3 is: SQLITE_USE_URI=0) reads the returned string as a PATH. Before the shape file:/forkdb/bN it was a legal
        # relative path, and the driver silently created a plain file named file:bN?vfs=forkdb: the application wrote into the wrong file and nothing failed. Now the path
        # has a directory, file:, that does not exist, so the open fails and creates nothing. Debian and Ubuntu build SQLite with URIs ON, so the child turns them off the way
        # a build with SQLITE_USE_URI=0 has them: sqlite3_config(SQLITE_CONFIG_URI, 0) on the shared library, before the sqlite3 module first uses it.
        c = self.load()
        self.new_store(c)
        uri = self.one(c, "select forkdb_open_db(?, 'main')", self.store)
        cwd = os.path.join(self.dir, "cwd")
        os.mkdir(cwd)
        code = ("import ctypes, ctypes.util, sys\n"
                "name = ctypes.util.find_library('sqlite3')\n"
                "rc = ctypes.CDLL(name).sqlite3_config(17, 0)\n"
                "if rc != 0:\n    print('CONFIG FAILED rc=%d lib=%s' % (rc, name)); sys.exit(4)\n"
                "import sqlite3\n"
                "try:\n    sqlite3.connect(sys.argv[1], uri=False)\nexcept sqlite3.OperationalError as e:\n    print('refused:', e); sys.exit(0)\n"
                "print('OPENED'); sys.exit(3)")
        r = subprocess.run([sys.executable, "-c", code, uri], capture_output=True, text=True, timeout=60, cwd=cwd)
        self.assertEqual(r.returncode, 0, "a plain open of the URI string must fail: %s%s" % (r.stdout, r.stderr))
        self.assertIn("unable to open database file", r.stdout)
        self.assertEqual(os.listdir(cwd), [], "the failed open must not create a file or directory")

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

    def test_a_store_older_than_format_version_4_is_refused_and_left_alone(self):
        # The message used to say "compact it with the forkdb CLI first": the CLI has no such step (and no store the CLI or the extension made has ever been older than version 4).
        data = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
        for name in ("v2-compacted.fk", "v3-compacted.fk"):
            with self.subTest(file=name):
                path = os.path.join(self.dir, name)
                shutil.copy(os.path.join(data, name), path)
                with open(path, "rb") as f:
                    before = f.read()
                out = self.check(path, "select forkdb_open_db(?, 'main')")
                self.assertIn("older than format version 4", out)
                self.assertNotIn("compact it with", out)
                with open(path, "rb") as f:
                    self.assertEqual(f.read(), before, "the file must be left as it was")

    def test_small_files_that_are_not_stores_are_never_overwritten(self):
        # Until this was fixed, any file under 4096 bytes (and a 4096-byte one with a bad header) was taken for a creation that a power cut interrupted and was OVERWRITTEN by an
        # empty store: forkdb_init on a 100-byte notes file, or on a one-page SQLite database, destroyed it. The 5000-byte file of the test below was one byte too long to see it.
        contents = {
            "text-100": b"my notes, do not lose me\n" * 4,
            "text-4095": b"x" * 4095,
            "text-4096": b"y" * 4096,
            "sqlite-4096": b"SQLite format 3\x00" + bytes(4080),
            "noise-300": bytes((i * 7 + 1) % 256 for i in range(300)),
        }
        funcs = [("select forkdb_init(?)", ())] + self.FUNCS
        for name, data in contents.items():
            for sql, _ in funcs:
                with self.subTest(file=name, sql=sql):
                    path = os.path.join(self.dir, name + ".fk")
                    with open(path, "wb") as f:
                        f.write(data)
                    self.check(path, sql)  # an error, not a crash (forkdb_diff stops earlier, at the missing session API: it never opens the file)
                    with open(path, "rb") as f:
                        self.assertEqual(f.read(), data, "%s on %s must leave the file alone" % (sql, name))

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


class H_NoSync(Base):
    """Tests for the nosync option: forkdb_init(path, 'main', 'nosync') skips fsync entirely."""

    @staticmethod
    def _read_count(tmpfile):
        with open(tmpfile, "r") as f:
            for line in f:
                if line.startswith("FSYNC_COUNT "):
                    return int(line.split()[1])
        return -1

    def run_counted(self, code, *args):
        """Run a subprocess with LD_PRELOAD counting fsync/fdatasync. Returns (CompletedProcess, count)."""
        so = os.environ.get("FK_FSYNC_COUNT_SO")
        if not so:
            self.fail("FK_FSYNC_COUNT_SO is not set: the fsync interposer is not available (run scripts/test-ext.sh)")
        if not os.path.exists(so):
            self.fail("FK_FSYNC_COUNT_SO=%s does not exist: build the interposer first (scripts/test-ext.sh)" % so)
        with tempfile.NamedTemporaryFile(mode="w+", suffix=".txt", delete=False) as tf:
            tmpfile = tf.name
        env = {**os.environ, "LD_PRELOAD": so, "FK_FSYNC_COUNT_FILE": tmpfile}
        r = subprocess.run(
            [sys.executable, "-c", PRELUDE + code, SO, *args],
            capture_output=True, text=True, timeout=120, env=env
        )
        count = self._read_count(tmpfile)
        os.unlink(tmpfile)
        return r, count

    # ---- subprocess scripts ----

    # These scripts use sys.argv[2] as the store path (P from PRELUDE),
    # sys.argv[3] as the root/branch name, and sys.argv[4] as the options word.
    # "none" means call forkdb_init(P) with only the path (no options).

    INIT_WORK_SCRIPT = """
c = load()
opt = sys.argv[4]
root = sys.argv[3]
if opt == 'none':
    c.execute("select forkdb_init(?, ?)", (P, root)).fetchall()
else:
    c.execute("select forkdb_init(?, ?, ?)", (P, root, opt)).fetchall()
uri = c.execute("select forkdb_open_db(?, ?)", (P, root)).fetchone()[0]
d = sqlite3.connect(uri, uri=True)
d.execute("create table t(a integer primary key, b text)")
d.execute("create index tb on t(b)")
for i in range(100):
    d.execute("insert into t values (?, ?)", (i, 'v%d' % i))
d.execute("commit")
for i in range(30):
    c.execute("select forkdb_fork(?, ?, ?)", (P, root, 't%d' % i)).fetchall()
    c.execute("select forkdb_discard(?, ?)", (P, 't%d' % i)).fetchall()
c.execute("select forkdb_close(?)", (P,)).fetchall()
"""

    INIT_WORK_SHORT_SCRIPT = """
c = load()
opt = sys.argv[4]
root = sys.argv[3]
if opt == 'none':
    c.execute("select forkdb_init(?, ?)", (P, root)).fetchall()
else:
    c.execute("select forkdb_init(?, ?, ?)", (P, root, opt)).fetchall()
c.execute("select forkdb_close(?)", (P,)).fetchall()
"""

    INIT_WORK_NAMED_SCRIPT = """
c = load()
opt = sys.argv[4]
root = sys.argv[3]
if opt == 'none':
    c.execute("select forkdb_init(?, ?)", (P, root)).fetchall()
else:
    c.execute("select forkdb_init(?, ?, ?)", (P, root, opt)).fetchall()
uri = c.execute("select forkdb_open_db(?, ?)", (P, root)).fetchone()[0]
d = sqlite3.connect(uri, uri=True)
d.execute("create table t(a integer primary key)")
for i in range(10):
    d.execute("insert into t values (?)", (i,))
d.execute("commit")
c.execute("select forkdb_close(?)", (P,)).fetchall()
"""

    NOSYNC_CRASH_SCRIPT = """
c = load()
c.execute("select forkdb_init(?, 'main', 'nosync')", (P,)).fetchall()
uri = c.execute("select forkdb_open_db(?, 'main')", (P,)).fetchone()[0]
d = sqlite3.connect(uri, uri=True)
for txn in range(20):
    d.execute("create table if not exists t(a integer primary key, b text)")
    count = d.execute("select ifnull(max(a), -1) + 1 from t").fetchone()[0]
    d.execute("insert into t values (?, 'row' || ?)", (count, count))
    d.execute("commit")
os._exit(9)
"""

    LOCK_SCRIPT = """
c = load()
try:
    c.execute("select forkdb_open_db(?, 'main')", (P,)).fetchone()
except sqlite3.OperationalError as e:
    print('error:', e)
    sys.exit(0)
print('NO ERROR')
sys.exit(3)
"""

    REOPEN_AFTER_NOSYNC_CLOSE = """
c = load()
uri = c.execute("select forkdb_open_db(?, 'main')", (P,)).fetchone()[0]
d = sqlite3.connect(uri, uri=True)
d.execute("insert into t values (100, 'durable-write')")
d.execute("commit")
d.close()
c.execute("select forkdb_close(?)", (P,)).fetchall()
"""

    # ---- tests ----

    def test_default_issues_syncs(self):
        path = os.path.join(self.dir, "def-syncs.fk")
        r, count = self.run_counted(self.INIT_WORK_SCRIPT, path, "main", "none")
        self.assertEqual(r.returncode, 0, "subprocess failed: rc=%s\n%s%s" % (r.returncode, r.stdout, r.stderr))
        self.assertGreaterEqual(count, 30, "default (durable) store should issue >= 30 fsyncs, got %d" % count)

    def test_portable_sync_counter_agrees_with_the_fsync_calls(self):
        # forkdb_sync_count() is what the macOS and Windows tests count with; here it is checked against the system calls the interposer sees, for a durable and a nosync store
        code = ("c0 = load()\n"
                "n0 = c0.execute('select forkdb_sync_count()').fetchone()[0]\n" + self.INIT_WORK_SCRIPT +
                "\nprint('PORTABLE', c0.execute('select forkdb_sync_count()').fetchone()[0] - n0)\n")
        for opt in ("none", "nosync"):
            with self.subTest(opt=opt):
                r, external = self.run_counted(code, os.path.join(self.dir, opt + ".fk"), "main", opt)
                self.assertEqual(r.returncode, 0, "subprocess failed: %s%s" % (r.stdout, r.stderr))
                portable = int(r.stdout.split("PORTABLE")[1])
                self.assertEqual(portable, external, "%s: forkdb_sync_count says %d, the interposer saw %d" % (opt, portable, external))
                if opt == "none":
                    self.assertGreater(portable, 0)
                else:
                    self.assertEqual(portable, 0)

    def test_nosync_issues_none(self):
        path = os.path.join(self.dir, "nosync-none.fk")
        r, count = self.run_counted(self.INIT_WORK_SCRIPT, path, "main", "nosync")
        self.assertEqual(r.returncode, 0, "subprocess failed: rc=%s\n%s%s" % (r.returncode, r.stdout, r.stderr))
        self.assertEqual(count, 0, "nosync store should issue exactly 0 fsyncs, got %d" % count)

    def test_nosync_with_other_words_and_separators(self):
        cases = [
            ("nosync, nosync", "nosync", "nosync with spaces and comma"),
            (",nosync", "nosync", "a leading comma"),
            ("  nosync  ,  ", "nosync", "runs of separators"),
            ("nosync,", "nosync", "nosync with trailing comma"),
            ("nosync nosync", "nosync", "two nosync words"),
        ]
        for opt, root, desc in cases:
            with self.subTest(opt=opt, root=root, desc=desc):
                path = os.path.join(self.dir, "var-%s.fk" % desc.replace(" ", "-"))
                r1, c1 = self.run_counted(self.INIT_WORK_SHORT_SCRIPT, path, root, opt)
                self.assertEqual(r1.returncode, 0, "failed with opt=%r root=%r: rc=%s" % (opt, root, r1.returncode))
                self.assertEqual(c1, 0, "nosync with opt=%r should give 0 fsyncs, got %d" % (opt, c1))
        # Also test with a root name other than 'main'
        path2 = os.path.join(self.dir, "trunk-nosync.fk")
        r2, c2 = self.run_counted(self.INIT_WORK_NAMED_SCRIPT, path2, "trunk", "nosync")
        self.assertEqual(r2.returncode, 0, "failed with trunk: rc=%s" % r2.returncode)
        self.assertEqual(c2, 0, "nosync with root 'trunk' should give 0 fsyncs, got %d" % c2)

    def test_unknown_option_is_an_error(self):
        for opt in ("nosnyc", "nosync bogus", "nosync,bogus", "nosync, bogus"):
            with self.subTest(opt=opt):
                path = os.path.join(self.dir, "bad-%s.fk" % opt.replace(" ", "-"))
                r, count = self.run_counted(self.INIT_WORK_SHORT_SCRIPT, path, "main", opt)
                self.assertNotEqual(r.returncode, 0, "expected error for opt=%r, got rc=0" % (opt,))
                self.assertIn("unknown option", r.stderr)
                if "bogus" in opt:
                    self.assertIn("bogus", r.stderr)
                self.assertIn("OperationalError", r.stderr)

    def test_nosync_data_survives_process_death(self):
        path = os.path.join(self.dir, "crash.fk")
        r = subprocess.run(
            [sys.executable, "-c", PRELUDE + self.NOSYNC_CRASH_SCRIPT, SO, path],
            capture_output=True, text=True, timeout=120
        )
        self.assertEqual(r.returncode, 9, "expected _exit(9), got rc=%s\n%s%s" % (r.returncode, r.stdout, r.stderr))
        # Now open in the parent process
        c = self.load()
        self.new_store(c, path=path)
        self.addCleanup(self.close_store, c, path)
        db = self.open_branch(c, path=path)
        rows = db.execute("select a from t order by a").fetchall()
        self.assertEqual(len(rows), 20, "expected 20 rows after crash, got %d" % len(rows))
        self.assertEqual(rows, [(i,) for i in range(20)])
        self.assertEqual(db.execute("pragma integrity_check").fetchall(), [("ok",)])

    def test_nosync_store_reopens_durably(self):
        # Write data under nosync, close cleanly, then open the store in a counted
        # subprocess (no init, just open_db). The reopened store should issue fsyncs
        # (it is durable unless opened nosync), and the nosync data must be intact.
        path = os.path.join(self.dir, "reopen.fk")

        # First a nosync session: write 30 rows, fork/discard a few times, close.
        r1 = run_py("""
c = load()
c.execute("select forkdb_init(?, 'main', 'nosync')", (P,)).fetchall()
uri = c.execute("select forkdb_open_db(?, 'main')", (P,)).fetchone()[0]
d = sqlite3.connect(uri, uri=True)
d.execute("create table t(a integer primary key, b text)")
for i in range(30):
    d.execute("insert into t values (?, ?)", (i, 'nosync-row-' + str(i)))
d.execute("commit")
for i in range(5):
    c.execute("select forkdb_fork(?, 'main', ?)", (P, 'f' + str(i))).fetchall()
    c.execute("select forkdb_discard(?, ?)", (P, 'f' + str(i))).fetchall()
c.execute("select forkdb_close(?)", (P,)).fetchall()
print('nosync session done')
""", path)
        self.assertEqual(r1.returncode, 0, "nosync session failed: rc=%s\n%s%s" % (r1.returncode, r1.stdout, r1.stderr))

        # Now a counted subprocess opens it without nosync (regular open_db), does a write.
        r2, count = self.run_counted(self.REOPEN_AFTER_NOSYNC_CLOSE, path)
        self.assertEqual(r2.returncode, 0, "durable reopen write failed: rc=%s\n%s%s" % (r2.returncode, r2.stdout, r2.stderr))
        self.assertGreaterEqual(count, 1, "reopening without nosync should issue >= 1 fsync, got %d" % count)

        # Verify data from nosync session is all there
        c = self.load()
        self.addCleanup(self.close_store, c, path)
        uri = self.one(c, "select forkdb_open_db(?, 'main')", path)
        db = sqlite3.connect(uri, uri=True)
        self.addCleanup(db.close)
        rows = db.execute("select a from t where a < 30 order by a").fetchall()
        self.assertEqual(len(rows), 30, "expected 30 nosync rows, got %d" % len(rows))
        self.assertEqual(db.execute("select a from t where a = 100").fetchone(), (100,))
        self.assertEqual(db.execute("pragma integrity_check").fetchall(), [("ok",)])

    COMPACT_SCRIPT = """
c = load()
opt = sys.argv[4]
root = sys.argv[3]
if opt == 'none':
    c.execute("select forkdb_init(?, ?)", (P, root)).fetchall()
else:
    c.execute("select forkdb_init(?, ?, ?)", (P, root, opt)).fetchall()
uri = c.execute("select forkdb_open_db(?, ?)", (P, root)).fetchone()[0]
d = sqlite3.connect(uri, uri=True)
d.execute("create table t(a integer primary key, b text)")
d.execute("create index tb on t(b)")
for i in range(2000):
    d.execute("insert into t values (?, ?)", (i, 'v%d' % i))
d.execute("commit")
# Fork, write and discard to create garbage, then compact
for i in range(20):
    c.execute("select forkdb_fork(?, ?, ?)", (P, root, 'f%d' % i)).fetchall()
    uri2 = c.execute("select forkdb_open_db(?, ?)", (P, 'f%d' % i)).fetchone()[0]
    d2 = sqlite3.connect(uri2, uri=True)
    d2.execute("update t set b = 'x' where a < 100")
    d2.execute("commit")
    d2.close()
    c.execute("select forkdb_discard(?, ?)", (P, 'f%d' % i)).fetchall()
# Compact with threshold 0 to force it
r = c.execute("select forkdb_compact(?)", (P,)).fetchone()[0]
c.execute("select forkdb_close(?)", (P,)).fetchall()
"""

    def test_nosync_compaction_issues_no_directory_sync(self):
        """Compaction under nosync must not issue any fsync, including the directory sync after rename."""
        path = os.path.join(self.dir, "nosync-compact.fk")
        r, count = self.run_counted(self.COMPACT_SCRIPT, path, "main", "nosync")
        self.assertEqual(r.returncode, 0, "nosync compaction failed: rc=%s\n%s%s" % (r.returncode, r.stdout, r.stderr))
        self.assertEqual(count, 0, "nosync compaction should issue exactly 0 fsyncs, got %d" % count)

    def test_nosync_store_is_still_locked(self):
        # Parent opens a nosync store and keeps it open.
        path = os.path.join(self.dir, "locked.fk")
        c = self.load()
        c.execute("select forkdb_init(?, 'main', 'nosync')", (path,)).fetchone()
        self.addCleanup(self.close_store, c, path)
        # Open a branch to hold the store open
        db = self.open_branch(c, path=path)
        db.execute("create table t(a)")
        db.execute("insert into t values (1)")
        db.execute("commit")

        # Subprocess tries to open the store
        r = subprocess.run(
            [sys.executable, "-c", PRELUDE + self.LOCK_SCRIPT, SO, path],
            capture_output=True, text=True, timeout=120
        )
        self.assertEqual(r.returncode, 0, "lock subprocess failed: rc=%s\n%s%s" % (r.returncode, r.stdout, r.stderr))
        self.assertIn("open in another process", r.stdout,
                      "expected 'open in another process' in stdout, got: %s" % r.stdout)

        # Parent data is fine
        self.assertEqual(db.execute("select count(*) from t").fetchone()[0], 1)


class I_Compaction(Base):
    """Compaction through the extension, in Python's sqlite3: the garbage of a discarded branch is reclaimed, and nothing else changes. Store-level compaction is covered
    in file_test.c and crash_test.c; this checks what the store's own tests cannot: the SQL layer, the VFS and live connections on top of it."""

    ROWS = 5000

    def make_table(self, c, opt=None):
        if opt:
            self.one(c, "select forkdb_init(?, 'main', ?)", self.store, opt)
        else:
            self.one(c, "select forkdb_init(?)", self.store)
        self.addCleanup(self.close_store, c, self.store)
        db = self.open_branch(c)
        db.execute("create table t(a integer primary key, b text)")
        db.executemany("insert into t values (?, ?)", ((i, "x" * 100) for i in range(self.ROWS)))
        db.commit()
        db.close()

    def make_garbage(self, c, name="junk"):
        # a branch that rewrites every page, then is discarded: most of the file is now garbage
        self.one(c, "select forkdb_fork(?, 'main', ?)", self.store, name)
        j = self.open_branch(c, name)
        j.execute("update t set b = b || 'abcdefghij'")
        j.commit()
        j.close()
        self.one(c, "select forkdb_discard(?, ?)", self.store, name)

    @staticmethod
    def digest(db):
        import hashlib
        h = hashlib.sha256()
        h.update(repr(db.execute("select sql from sqlite_master order by name").fetchall()).encode())
        h.update(repr(db.execute("select * from t order by a").fetchall()).encode())
        return h.hexdigest()

    def uri(self, c, ref):
        return self.one(c, "select forkdb_open_db(?, ?)", self.store, ref)

    def digest_of(self, c, ref):
        # through a fresh connection; integrity_check must be ok whatever the branch holds
        db = sqlite3.connect(self.uri(c, ref).replace("?vfs", "?mode=ro&vfs"), uri=True)  # read-only: a snapshot cannot be opened any other way
        try:
            self.assertEqual(db.execute("pragma integrity_check").fetchall(), [("ok",)], "integrity_check on %s" % ref)
            return self.digest(db)
        finally:
            db.close()

    def size(self):
        return os.path.getsize(self.store)

    def compact(self, c):
        return self.one(c, "select forkdb_compact(?)", self.store)

    def test_compaction_compacts_and_shrinks(self):
        c = self.load()
        self.make_table(c)
        self.make_garbage(c)
        before = self.size()
        self.assertEqual(self.compact(c), 1, "a file that is mostly garbage must be compacted")
        after = self.size()  # measured now: the next write extends the file by a preallocation chunk again
        self.assertLess(after * 4, before, "the file went from %d to %d bytes" % (before, after))
        self.assertEqual(self.compact(c), 0, "a second compaction has nothing to do")
        self.assertEqual(self.size(), after)

    def make_branches(self, c):
        """main (edited), a, b (shrinks and grows back), c (forked from a), snap (a snapshot of main), then a discarded junk branch."""
        self.make_table(c)
        self.one(c, "select forkdb_fork(?, 'main', 'a')", self.store)
        a = self.open_branch(c, "a")
        a.execute("update t set b = 'A' where a < 200")
        a.commit()
        a.close()
        self.one(c, "select forkdb_fork(?, 'main', 'b')", self.store)
        b = self.open_branch(c, "b")
        b.execute("delete from t where a >= 4000")
        b.commit()
        b.execute("vacuum")  # the branch gets smaller ...
        b.executemany("insert into t values (?, ?)", ((10000 + i, "y" * 100) for i in range(500)))
        b.commit()  # ... and then grows again
        b.close()
        self.one(c, "select forkdb_fork(?, 'a', 'c')", self.store)
        k = self.open_branch(c, "c")
        k.execute("update t set b = 'C' where a >= 200 and a < 300")
        k.commit()
        k.close()
        m = self.open_branch(c, "main")
        m.execute("update t set b = 'M' where a >= 4900")
        m.commit()
        m.close()
        self.one(c, "select forkdb_snapshot(?, 'main', 'snap')", self.store)
        m = self.open_branch(c, "main")
        m.execute("update t set b = 'N' where a >= 4800 and a < 4850")  # main moves on; the snapshot stays frozen
        m.commit()
        m.close()
        self.make_garbage(c)

    NAMES = ("main", "a", "b", "c", "snap")

    def test_every_branch_reads_the_same_after_compaction(self):
        c = self.load()
        self.make_branches(c)
        before = {n: self.digest_of(c, n) for n in self.NAMES}
        self.assertEqual(len(set(before.values())), len(self.NAMES), "the branches were made to differ")
        uris = {n: self.uri(c, n) for n in self.NAMES}
        self.assertEqual(self.compact(c), 1)
        for n in self.NAMES:
            self.assertEqual(self.digest_of(c, n), before[n], "branch %s changed through the compaction" % n)
            self.assertEqual(self.uri(c, n), uris[n], "the store id of branch %s changed" % n)
        self.assertIn("no branch named", self.err(c, "select forkdb_open_db(?, 'junk')", self.store))
        m = self.open_branch(c, "main")
        self.assertEqual(m.execute("select count(*) from t where b = 'A' or b = 'C'").fetchone()[0], 0, "main must not see what a and c wrote")

    def test_branches_stay_isolated_and_writable_after_compaction(self):
        c = self.load()
        self.make_branches(c)
        before = {n: self.digest_of(c, n) for n in self.NAMES}
        self.assertEqual(self.compact(c), 1)
        a = self.open_branch(c, "a")
        a.execute("update t set b = 'A2' where a >= 1000 and a < 1100")
        a.commit()
        a.close()
        self.one(c, "select forkdb_fork(?, 'main', 'fresh')", self.store)
        self.assertEqual(self.digest_of(c, "fresh"), before["main"], "a fork after the compaction holds what main holds")
        f = self.open_branch(c, "fresh")
        f.execute("delete from t")
        f.commit()
        f.close()
        for n in ("main", "b", "c", "snap"):
            self.assertEqual(self.digest_of(c, n), before[n], "branch %s changed through writes to other branches" % n)
        self.assertNotEqual(self.digest_of(c, "a"), before["a"])
        # a snapshot is read-only, before and after
        with self.assertRaises(sqlite3.OperationalError) as cm:
            sqlite3.connect(self.uri(c, "snap"), uri=True)
        self.assertIn("readonly", str(cm.exception))

    def test_compaction_with_connections_open(self):
        c = self.load()
        self.make_table(c)
        for n in ("keep", "w"):
            self.one(c, "select forkdb_fork(?, 'main', ?)", self.store, n)
        keep_before = self.digest_of(c, "keep")
        main_before = self.digest_of(c, "main")
        self.make_garbage(c)

        def conn(ref):
            return sqlite3.connect(self.uri(c, ref), uri=True, isolation_level=None)

        w = conn("w")
        w.execute("begin immediate")
        w.execute("update t set b = 'W' where a < 500")  # an uncommitted write transaction
        k = conn("keep")
        cur = k.execute("select a, b from t order by a")
        first = cur.fetchmany(100)  # a half-read cursor
        rd = conn("main")
        rd.execute("begin")
        rd.execute("select count(*) from t").fetchall()  # an open read transaction
        idle = conn("main")
        for x in (w, k, rd, idle):
            self.addCleanup(x.close)

        self.assertEqual(self.compact(c), 1, "compaction with live connections")
        rest = cur.fetchall()
        self.assertEqual(len(first) + len(rest), self.ROWS, "the cursor continues across the compaction")
        self.assertTrue(all(b == "x" * 100 for _, b in first + rest))
        w.execute("update t set b = 'W2' where a >= 500 and a < 600")
        w.execute("commit")  # the writer finishes after the compaction
        self.assertEqual(w.execute("select count(*) from t where b = 'W'").fetchone()[0], 500)
        self.assertEqual(w.execute("select count(*) from t where b = 'W2'").fetchone()[0], 100)
        self.assertEqual(rd.execute("select count(*) from t where b = 'W' or b = 'W2'").fetchone()[0], 0, "main must not see the writer's rows")
        self.assertEqual(idle.execute("select count(*) from t").fetchone()[0], self.ROWS)
        for x in (w, k, rd, idle):
            self.assertEqual(x.execute("pragma integrity_check").fetchall(), [("ok",)])
        for x in (w, k, rd, idle):
            x.close()
        self.one(c, "select forkdb_close(?)", self.store)
        self.assertEqual(self.digest_of(c, "main"), main_before)
        self.assertEqual(self.digest_of(c, "keep"), keep_before)
        self.assertEqual(self.digest_of(c, "w") == main_before, False)

    def test_repeated_cycles_return_to_the_live_size(self):
        c = self.load()
        self.make_table(c)
        main_before = self.digest_of(c, "main")
        sizes = []
        for i in range(12):
            self.make_garbage(c)
            self.assertEqual(self.compact(c), 1, "cycle %d" % i)
            sizes.append(self.size())  # right after the compaction, before any further write
        self.assertLessEqual(max(sizes), min(sizes) * 1.15, "file sizes after each compaction: %r" % sizes)
        self.assertEqual(self.digest_of(c, "main"), main_before)

    DIGESTS = """
import hashlib
c = load()
out = []
for ref in sys.argv[3:]:
    d = sqlite3.connect(c.execute("select forkdb_open_db(?, ?)", (P, ref)).fetchone()[0].replace("?vfs", "?mode=ro&vfs"), uri=True)
    h = hashlib.sha256()
    h.update(repr(d.execute("select sql from sqlite_master order by name").fetchall()).encode())
    h.update(repr(d.execute("select * from t order by a").fetchall()).encode())
    out.append(h.hexdigest() + " " + d.execute("pragma integrity_check").fetchone()[0])
    d.close()
print("|".join(out))
"""

    def check_in_another_process(self, c, names, before):
        self.one(c, "select forkdb_close(?)", self.store)
        r = run_py(self.DIGESTS, self.store, *names)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.strip().split("|"), [before[n] + " ok" for n in names])

    def test_compaction_survives_reopen_and_other_processes(self):
        c = self.load()
        self.make_branches(c)
        before = {n: self.digest_of(c, n) for n in self.NAMES}
        self.assertEqual(self.compact(c), 1)
        self.check_in_another_process(c, self.NAMES, before)

    def test_compaction_under_nosync_reopens_durably(self):
        c = self.load()
        self.make_table(c, opt="nosync")
        self.make_garbage(c)
        before = {"main": self.digest_of(c, "main")}
        self.assertEqual(self.compact(c), 1)
        self.check_in_another_process(c, ("main",), before)  # the other process opens it without the option

    run_counted = H_NoSync.run_counted  # the LD_PRELOAD fsync counter, same helper
    _read_count = staticmethod(H_NoSync._read_count)

    COUNT_SCRIPT = """
c = load()
opt, step = sys.argv[3], sys.argv[4]
if opt == 'none':
    c.execute("select forkdb_init(?)", (P,)).fetchall()
else:
    c.execute("select forkdb_init(?, 'main', ?)", (P, opt)).fetchall()
d = sqlite3.connect(c.execute("select forkdb_open_db(?, 'main')", (P,)).fetchone()[0], uri=True)
d.execute("create table t(a integer primary key, b text)")
d.executemany("insert into t values (?, ?)", ((i, 'x' * 100) for i in range(5000)))
d.commit()
d.close()
c.execute("select forkdb_fork(?, 'main', 'junk')", (P,)).fetchall()
j = sqlite3.connect(c.execute("select forkdb_open_db(?, 'junk')", (P,)).fetchone()[0], uri=True)
j.execute("update t set b = b || 'abcdefghij'")
j.commit()
j.close()
c.execute("select forkdb_discard(?, 'junk')", (P,)).fetchall()
if step == 'compact':
    print('compacted', c.execute("select forkdb_compact(?)", (P,)).fetchone()[0])
else:
    print('compacted skipped')
c.execute("select forkdb_close(?)", (P,)).fetchall()
"""

    def test_compaction_sync_counts(self):
        counts = {}
        for opt in ("none", "nosync"):
            for step in ("compact", "skip"):
                r, n = self.run_counted(self.COUNT_SCRIPT, os.path.join(self.dir, "%s-%s.fk" % (opt, step)), opt, step)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                want = "compacted 1" if step == "compact" else "compacted skipped"
                self.assertEqual(r.stdout.strip(), want, "the compaction must really happen (the sync count of a compaction that did not run proves nothing)")
                counts[(opt, step)] = n
        self.assertEqual(counts[("nosync", "compact")], 0, "a nosync store issues no fsync, compaction included: %r" % counts)
        self.assertEqual(counts[("nosync", "skip")], 0, counts)
        self.assertGreater(counts[("none", "skip")], 0, counts)
        self.assertGreater(counts[("none", "compact")], counts[("none", "skip")], "compaction itself syncs a durable store: %r" % counts)

    THREADED = """
import threading, time, random
host = sqlite3.connect(':memory:', check_same_thread=False)
host.enable_load_extension(True)
host.load_extension(SO)
host.execute("select forkdb_init(?)", (P,)).fetchall()

def conn(ref):
    uri = host.execute("select forkdb_open_db(?, ?)", (P, ref)).fetchone()[0]
    return sqlite3.connect(uri, uri=True, check_same_thread=False, timeout=10, isolation_level=None)

m = conn('main')
m.execute("create table t(a integer primary key, b text)")
m.execute("begin")
for i in range(5000):
    m.execute("insert into t values (?, ?)", (i, 'x' * 100))
m.execute("commit")
m.close()
for n in ('a', 'b', 'c'):
    host.execute("select forkdb_fork(?, 'main', ?)", (P, n)).fetchall()
conns = [conn(n) for n in ('a', 'b', 'c')]
ok = [0, 0, 0]
errs = []

def writer(i):
    c = conns[i]
    base = 100000 * (i + 1)
    for k in range(150):
        for attempt in range(600):
            try:
                c.execute('begin immediate')
                c.execute("insert into t values (?, 'w')", (base + k,))
                c.execute('commit')
                ok[i] += 1
                break
            except sqlite3.OperationalError as e:
                try:
                    c.execute('rollback')
                except sqlite3.Error:
                    pass
                if 'locked' in str(e) or 'busy' in str(e):
                    time.sleep(random.random() / 500)
                    continue
                errs.append('writer %d: %s' % (i, e))
                return

compactions = [0]

def manager():
    for k in range(6):
        try:
            host.execute("select forkdb_fork(?, 'main', 'junk')", (P,)).fetchall()
            j = conn('junk')
            j.execute("update t set b = b || 'abcdefghij'")
            j.close()
            host.execute("select forkdb_discard(?, 'junk')", (P,)).fetchall()
            compactions[0] += host.execute("select forkdb_compact(?)", (P,)).fetchone()[0]
        except Exception as e:
            errs.append('manager: %s: %s' % (type(e).__name__, e))
            return

ts = [threading.Thread(target=writer, args=(i,)) for i in range(3)] + [threading.Thread(target=manager)]
for t in ts:
    t.start()
for t in ts:
    t.join()
bad = list(errs)
if compactions[0] < 3:
    bad.append('only %d compactions returned 1' % compactions[0])
for i, c in enumerate(conns):
    n = c.execute('select count(*) from t').fetchone()[0]
    if n != 5000 + ok[i]:
        bad.append('branch %d: %d rows, expected %d' % (i, n, 5000 + ok[i]))
    if c.execute('pragma integrity_check').fetchall() != [('ok',)]:
        bad.append('integrity_check on branch %d' % i)
m = conn('main')
if m.execute('select count(*) from t').fetchone()[0] != 5000 or m.execute("select count(*) from t where b = ?", ('x' * 100,)).fetchone()[0] != 5000:
    bad.append('main changed')
for c in conns + [m]:
    c.close()
host.execute("select forkdb_close(?)", (P,)).fetchall()
for i, n in enumerate(('a', 'b', 'c')):
    c = conn(n)
    cnt = c.execute('select count(*) from t').fetchone()[0]
    if cnt != 5000 + ok[i]:
        bad.append('after reopening, branch %s has %d rows, expected %d' % (n, cnt, 5000 + ok[i]))
    c.close()
print('commits', ok, 'compactions', compactions[0], 'problems', bad[:5])
print('ok' if not bad else 'BAD')
sys.exit(1 if bad else 0)
"""

    def test_compaction_while_threads_write(self):
        r = run_py(self.THREADED, os.path.join(self.dir, "s.fk"))
        self.assertEqual(r.returncode, 0, "rc=%s\\n%s%s" % (r.returncode, r.stdout, r.stderr))
        self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)


if __name__ == "__main__":
    unittest.main()
