"""pytest plugin: every test gets its own branch of a seeded SQLite database (forkdb), instead of a file copy or a rebuilt schema.

How it works
  * Once per process (per xdist worker) a store is created and the 'forkdb_seed' callable fills its root branch; a read-only snapshot ('seed') freezes
    that state. Seeding cost is paid once.
  * Per test, forkdb_fork makes a branch from the snapshot (constant time: it copies no pages), the test gets a connection (and a URI, for code that opens
    its own connection), and the branch is discarded afterwards, pass or fail.
  * Nothing a test writes is visible to another test, to the seed or to a later run.

Requirements: Python's sqlite3 must allow load_extension, and build/ext/forkdb.so (scripts/build-ext.sh) or FORKDB_EXT must name the library.
One process holds a store at a time (the file is locked): under pytest-xdist every worker gets its own store, in its own temporary directory.

Fixtures
  forkdb_seed   (session, override it)  callable(conn) that creates the schema and the data every test starts from
  forkdb_configure (session, override)  callable(conn) applied to every connection 'db' hands out (row_factory, PRAGMA foreign_keys)
  db_uri        (function)              URI of this test's branch; pass it to code that connects by itself: sqlite3.connect(db_uri, uri=True)
  db            (function)              an open sqlite3.Connection on this test's branch
  forkdb_stats  (session)               counters: forks, discards, seconds spent forking and discarding, seed seconds
"""
import itertools
import os
import sqlite3
import time

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_SO = os.path.normpath(os.path.join(_HERE, "..", "..", "build", "ext", "forkdb.so"))


def _load():
    so = os.environ.get("FORKDB_EXT") or _DEFAULT_SO
    if not os.path.exists(so):
        raise pytest.UsageError("forkdb extension not found at %s: run scripts/build-ext.sh or set FORKDB_EXT" % so)
    c = sqlite3.connect(":memory:", check_same_thread=False)
    if not hasattr(c, "enable_load_extension"):
        raise pytest.UsageError("this Python's sqlite3 cannot load extensions")
    c.enable_load_extension(True)
    c.load_extension(so)
    return c


def pytest_addoption(parser):
    g = parser.getgroup("forkdb")
    g.addoption("--forkdb-dir", default=os.environ.get("FORKDB_DIR"),
                help="directory for the stores (one subdirectory per xdist worker). A tmpfs such as /dev/shm makes fork and discard ~0.01 ms instead of "
                     "~4 ms each: on a disk both end in an fsync. Default: pytest's temporary directory (env FORKDB_DIR).")


    g.addoption("--forkdb-max-mb", type=int, default=int(os.environ.get("FORKDB_MAX_MB", "64")),
                help="compact the store when its file passes this size (default 64). Discarding a branch does not give its pages back: a suite whose tests "
                     "commit grew the file by ~30 KB per test (measured), ~0.5 MB for tests that rewrite thousands of rows, until forkdb_compact ran.")


class _Stats(dict):
    pass


class ForkStore:
    """The session's store: a host connection (which carries the forkdb_* functions) and the path."""

    def __init__(self, path, seed, max_mb=64):
        self.path = path
        self.host = _load()
        self.max_bytes = max_mb * 1024 * 1024
        self.stats = _Stats(forks=0, discards=0, compactions=0, compact_seconds=0.0, fork_seconds=0.0, discard_seconds=0.0, seed_seconds=0.0)
        self._ids = itertools.count(1)
        t0 = time.perf_counter()
        self.host.execute("select forkdb_init(?, 'main')", (path,)).fetchall()
        conn = self.connect("main")
        try:
            seed(conn)
            conn.commit()
        finally:
            conn.close()
        self.host.execute("select forkdb_snapshot(?, 'main', 'seed')", (path,)).fetchall()
        self.stats["seed_seconds"] = time.perf_counter() - t0

    def uri(self, ref):
        # The URI names the branch by a store id that is only valid until the next fork, discard or reset: open it at once.
        return self.host.execute("select forkdb_open_db(?, ?)", (self.path, ref)).fetchone()[0]

    def connect(self, ref):
        return sqlite3.connect(self.uri(ref), uri=True, check_same_thread=False)

    def new_branch(self):
        name = "t%d" % next(self._ids)
        t0 = time.perf_counter()
        self.host.execute("select forkdb_fork(?, 'seed', ?)", (self.path, name)).fetchall()
        self.stats["fork_seconds"] += time.perf_counter() - t0
        self.stats["forks"] += 1
        return name

    def drop_branch(self, name):
        t0 = time.perf_counter()
        self.host.execute("select forkdb_discard(?, ?)", (self.path, name)).fetchall()
        self.stats["discard_seconds"] += time.perf_counter() - t0
        self.stats["discards"] += 1
        if os.path.getsize(self.path) > self.max_bytes:
            # No connection is open on the store here: the test's was closed before this discard, and nothing else holds one.
            t0 = time.perf_counter()
            self.host.execute("select forkdb_compact(?)", (self.path,)).fetchall()
            self.stats["compact_seconds"] += time.perf_counter() - t0
            self.stats["compactions"] += 1

    def close(self):
        self.host.execute("select forkdb_close(?)", (self.path,)).fetchall()
        self.host.close()


@pytest.fixture(scope="session")
def forkdb_seed():
    """Override in conftest.py: return a callable(conn) that creates the schema and the seed data."""
    return lambda conn: None


@pytest.fixture(scope="session")
def forkdb_configure():
    """Override in conftest.py: return a callable(conn) applied to every connection the 'db' fixture opens (row_factory, PRAGMA foreign_keys, ...)."""
    return lambda conn: None


@pytest.fixture(scope="session")
def forkdb_store(request, tmp_path_factory, forkdb_seed):
    base = request.config.getoption("--forkdb-dir")
    if base:
        os.makedirs(base, exist_ok=True)
        import tempfile
        path = os.path.join(tempfile.mkdtemp(prefix="fk-pytest-", dir=base), "seed.fk")  # own directory: removed below
    else:
        path = str(tmp_path_factory.mktemp("forkdb") / "seed.fk")
    store = ForkStore(path, forkdb_seed, request.config.getoption("--forkdb-max-mb"))
    yield store
    store.close()
    if base:
        import shutil
        shutil.rmtree(os.path.dirname(path), ignore_errors=True)


@pytest.fixture(scope="session")
def forkdb_stats(forkdb_store):
    return forkdb_store.stats


@pytest.fixture
def _branch(forkdb_store):
    name = forkdb_store.new_branch()
    try:
        yield name
    finally:
        forkdb_store.drop_branch(name)


@pytest.fixture
def db_uri(forkdb_store, _branch):
    return forkdb_store.uri(_branch)


@pytest.fixture
def db(forkdb_store, _branch, forkdb_configure):
    conn = forkdb_store.connect(_branch)
    forkdb_configure(conn)
    try:
        yield conn
    finally:
        conn.close()  # before the branch is discarded: a branch with an open connection cannot be discarded
