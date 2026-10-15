"""Scale benchmark: the same tests under different isolation strategies. SCALE_PLUGIN=forkdb (default) | copy | reseed; SCALE_ROWS sets the seed size."""
import os
import sys

import pytest

_PLUGIN = os.environ.get("SCALE_PLUGIN", "forkdb")
pytest_plugins = ["forkdb_pytest"] if _PLUGIN.startswith("forkdb") else ["baseline_plugins"]  # forkdb: no fsync (the plugin default); forkdb-durable: fsync
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROWS = int(os.environ.get("SCALE_ROWS", "200000"))


@pytest.fixture(scope="session")
def forkdb_seed():
    def seed(conn):
        conn.execute("create table events(id integer primary key, kind integer, payload text)")
        conn.execute("create index ek on events(kind)")
        conn.executemany("insert into events values (?, ?, ?)", ((i, i % 50, "x" * 100) for i in range(ROWS)))
    return seed
