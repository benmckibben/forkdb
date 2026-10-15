"""Baseline isolation strategies with the same fixtures as forkdb_pytest, for comparison only (scale/run_scale.sh):
  SCALE_PLUGIN=copy     copy a seeded database file per test
  SCALE_PLUGIN=reseed   run the seed function into a new file per test (what a conftest that builds its schema per test does)
"""
import os
import shutil
import sqlite3
import tempfile

import pytest

MODE = os.environ.get("SCALE_PLUGIN", "copy")


def pytest_addoption(parser):
    g = parser.getgroup("forkdb")
    g.addoption("--forkdb-dir", default=None)


@pytest.fixture(scope="session")
def forkdb_seed():
    return lambda conn: None


@pytest.fixture(scope="session")
def forkdb_configure():
    return lambda conn: None


class Store:
    def __init__(self, base, seed):
        if base:
            os.makedirs(base, exist_ok=True)
        self.dir = tempfile.mkdtemp(prefix="fk-base-", dir=base)
        self.seed = seed
        self.master = os.path.join(self.dir, "master.db")
        c = sqlite3.connect(self.master)
        seed(c)
        c.commit()
        c.close()
        self.n = 0


@pytest.fixture(scope="session")
def forkdb_store(request, forkdb_seed):
    s = Store(request.config.getoption("--forkdb-dir"), forkdb_seed)
    yield s
    shutil.rmtree(s.dir, ignore_errors=True)


@pytest.fixture
def db_uri(forkdb_store):
    s = forkdb_store
    s.n += 1
    p = os.path.join(s.dir, "t%d.db" % s.n)
    if MODE == "copy":
        shutil.copyfile(s.master, p)
    else:
        c = sqlite3.connect(p)
        s.seed(c)
        c.commit()
        c.close()
    yield "file:%s" % p
    os.unlink(p)


@pytest.fixture
def db(db_uri, forkdb_configure):
    c = sqlite3.connect(db_uri, uri=True)
    forkdb_configure(c)
    yield c
    c.close()
