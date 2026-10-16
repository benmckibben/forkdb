# forkdb for Python

The [forkdb](https://github.com/benmckibben/forkdb) SQLite extension, prebuilt, plus a few lines of Python to load it. **Unstable first release (0.1.0): everything may change** (see docs/STABILITY.md in the repository).

    pip install forkdb

```python
import forkdb
host = forkdb.connect()                       # sqlite3.connect(':memory:', uri=True) with the extension loaded
host.execute('select forkdb_init(?)', ('/tmp/s.fk',))
main = forkdb.open_branch(host, '/tmp/s.fk')   # the branch 'main'
host.execute("select forkdb_fork(?, 'main', 'a')", ('/tmp/s.fk',))
a = forkdb.open_branch(host, '/tmp/s.fk', 'a')
```

Wheels exist for Linux x86_64 (manylinux_2_28), macOS arm64 and Windows x86_64, and carry no dependency on the Python version. Elsewhere, build the extension from the repository and point `FORKDB_EXTENSION_PATH` at it.
Your Python's `sqlite3` must support loading extensions (`Connection.enable_load_extension`); `forkdb.load` says so if it does not. The SQLite behind it must be 3.24 or later.
