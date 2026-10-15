"""Isolation canaries for the forkdb pytest plugin. Run: pytest canary  (FORKDB_ORDER=reverse|shuffle changes the order; -n 4 works)."""
import os
import random

import pytest

pytest_plugins = ["forkdb_pytest"]

SEED_ROWS = 500


@pytest.fixture(scope="session")
def forkdb_seed():
    def seed(conn):
        conn.execute("create table t(a integer primary key, b text)")
        conn.execute("create table u(a integer primary key)")
        conn.executemany("insert into t values (?, 'seed')", ((i,) for i in range(SEED_ROWS)))
        conn.executemany("insert into u values (?)", ((i,) for i in range(SEED_ROWS)))
    return seed


def pytest_collection_modifyitems(items):
    order = os.environ.get("FORKDB_ORDER")
    if order == "reverse":
        items.reverse()
    elif order == "shuffle":
        random.Random(int(os.environ.get("FORKDB_SEED", "1"))).shuffle(items)
