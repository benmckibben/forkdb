"""forkdb: branch-per-test SQLite databases (the forkdb loadable extension, prebuilt).

    import forkdb
    host = forkdb.connect()                      # an in-memory connection with the extension loaded
    host.execute("select forkdb_init(?)", (path,))
    main = forkdb.open_branch(host, path)        # a connection to the branch 'main'
    host.execute("select forkdb_fork(?, 'main', 'a')", (path,))
    a = forkdb.open_branch(host, path, "a")

The extension is a loadable SQLite extension; this package only carries it and loads it. See https://github.com/benmckibben/forkdb
"""
import os
import sqlite3
import sys

__version__ = "0.1.1"
__all__ = ["extension_path", "load", "connect", "open_branch", "__version__"]

_SUFFIX = {"win32": ".dll", "darwin": ".dylib"}.get(sys.platform, ".so")


def extension_path():
    """The path of the forkdb library for this platform: the one bundled in the wheel, or the file named by FORKDB_EXTENSION_PATH."""
    override = os.environ.get("FORKDB_EXTENSION_PATH")
    if override:
        if not os.path.isfile(override):
            raise FileNotFoundError("FORKDB_EXTENSION_PATH=%s does not exist" % override)
        return override
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_lib", "forkdb" + _SUFFIX)
    if not os.path.isfile(here):
        raise FileNotFoundError(
            "this install of forkdb has no library for %s (%s): expected %s. Wheels are built for Linux x86_64, macOS arm64 and Windows x86_64; "
            "elsewhere build the extension (scripts/build-ext.sh in the repository) and set FORKDB_EXTENSION_PATH." % (sys.platform, sys.version.split()[0], here))
    return here


def load(conn):
    """Load the forkdb extension into a sqlite3 connection. Extension loading is switched back off afterwards."""
    if not hasattr(conn, "enable_load_extension"):
        raise RuntimeError(
            "this Python's sqlite3 module was built without extension loading (Connection.enable_load_extension is missing), so forkdb cannot be loaded. "
            "Use a Python whose sqlite3 supports it (python.org installers, most Linux distributions, the Windows and macOS builds of actions/setup-python); "
            "the macOS system Python and some pyenv builds do not.")
    path = extension_path()
    conn.enable_load_extension(True)
    try:
        conn.load_extension(path)
    finally:
        conn.enable_load_extension(False)
    return conn


def connect(database=":memory:", **kwargs):
    """sqlite3.connect() with uri=True and the extension loaded. The returned connection is the 'host' that forkdb_* functions are called on;
    it need not be the one that holds your data (see open_branch)."""
    kwargs.setdefault("uri", True)
    kwargs.setdefault("check_same_thread", False)
    return load(sqlite3.connect(database, **kwargs))


def open_branch(host, store, ref="main", readonly=False, **kwargs):
    """A connection to the branch (or snapshot) named ref of the store at path store. forkdb_open_db returns a URI, which sqlite3 only understands with uri=True;
    snapshots can only be opened read-only (readonly=True)."""
    uri = host.execute("select forkdb_open_db(?, ?)", (store, ref)).fetchone()[0]
    if readonly:
        uri = uri.replace("?vfs=", "?mode=ro&vfs=", 1)
    return sqlite3.connect(uri, uri=True, **kwargs)
