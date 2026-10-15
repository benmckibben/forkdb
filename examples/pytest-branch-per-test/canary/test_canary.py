"""Every test below either wrecks its database or checks that nothing of the sort happened before it. They are written to fail loudly if one test's
writes can be seen by another, by order, by repetition, or by a neighbour running at the same time in another xdist worker."""
import os
import sqlite3

import pytest

from conftest import SEED_ROWS


def seed_is_intact(conn):
    assert conn.execute("select count(*), sum(a) from t").fetchone() == (SEED_ROWS, sum(range(SEED_ROWS)))
    assert conn.execute("select count(*) from t where b != 'seed'").fetchone()[0] == 0
    assert conn.execute("select count(*) from u").fetchone()[0] == SEED_ROWS
    assert conn.execute("select name from sqlite_master where type = 'table' order by name").fetchall() == [("t",), ("u",)]


@pytest.mark.parametrize("n", range(100))
def test_check_seed(db, n):
    seed_is_intact(db)


@pytest.mark.parametrize("how", ["delete-all", "drop-table", "update-all", "junk-rows", "open-transaction", "new-table", "rollback-only", "vacuum-ish"])
@pytest.mark.parametrize("n", range(10))
def test_destroy(db, how, n):
    seed_is_intact(db)
    if how == "delete-all":
        db.execute("delete from t"); db.execute("delete from u"); db.commit()
    elif how == "drop-table":
        db.execute("drop table t"); db.commit()
    elif how == "update-all":
        db.execute("update t set b = 'wrecked'"); db.commit()
    elif how == "junk-rows":
        db.executemany("insert into t values (?, 'junk')", ((100000 + i,) for i in range(20000))); db.commit()
    elif how == "open-transaction":
        db.execute("update t set b = 'never committed'")  # the connection is closed with the transaction open
    elif how == "new-table":
        db.execute("create table leftover(x)"); db.execute("insert into leftover values (1)"); db.commit()
    elif how == "rollback-only":
        db.execute("delete from t"); db.rollback()
    elif how == "vacuum-ish":
        db.execute("delete from t where a % 2 = 0"); db.commit(); db.execute("vacuum")


def test_two_connections_on_one_branch_see_each_other(db, db_uri):
    other = sqlite3.connect(db_uri, uri=True)
    try:
        db.execute("insert into t values (-1, 'mine')"); db.commit()
        assert other.execute("select count(*) from t where a = -1").fetchone()[0] == 1
        other.execute("delete from t where a = -1"); other.commit()
        assert db.execute("select count(*) from t where a = -1").fetchone()[0] == 0
    finally:
        other.close()
    db.execute("delete from t"); db.commit()


def test_worker_marker_is_private(db, forkdb_store):
    # each xdist worker owns its store: two workers never share a file (the file is locked), and a marker written here is never seen elsewhere
    db.execute("create table marker(w text)"); db.execute("insert into marker values (?)", (os.environ.get("PYTEST_XDIST_WORKER", "main"),)); db.commit()
    assert db.execute("select count(*) from marker").fetchone()[0] == 1


@pytest.mark.parametrize("n", range(100))
def test_check_seed_again(db, n):
    seed_is_intact(db)


def test_no_branch_outlives_its_test(db, forkdb_store, forkdb_stats):
    # wherever this test runs, in any order and in any worker: every branch made for an earlier test is gone from the store (asked of the store, not of the
    # plugin's counters), and this test's own is the only one alive
    assert forkdb_stats["forks"] == forkdb_stats["discards"] + 1, forkdb_stats
    for k in range(1, forkdb_stats["forks"]):
        with pytest.raises(sqlite3.OperationalError, match="no branch named"):
            forkdb_store.host.execute("select forkdb_open_db(?, ?)", (forkdb_store.path, "t%d" % k)).fetchall()
    assert forkdb_store.host.execute("select forkdb_open_db(?, ?)", (forkdb_store.path, "t%d" % forkdb_stats["forks"])).fetchone()[0].startswith("file:b")
