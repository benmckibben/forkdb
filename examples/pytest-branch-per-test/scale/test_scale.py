"""200 ordinary-looking tests against a large seeded database: a read, a write that is committed, a write that is not."""
import pytest

from conftest import ROWS


@pytest.mark.parametrize("n", range(100))
def test_reads_the_seed(db, n):
    assert db.execute("select count(*) from events where kind = ?", (n % 50,)).fetchone()[0] == ROWS // 50
    assert db.execute("select count(*) from events where id < 100").fetchone()[0] == 100


@pytest.mark.parametrize("n", range(60))
def test_writes_and_commits(db, n):
    db.execute("update events set payload = 'changed' where id < 50")
    db.execute("insert into events values (?, 99, 'new')", (ROWS + n,))
    db.commit()
    assert db.execute("select count(*) from events where kind = 99").fetchone()[0] == 1


@pytest.mark.parametrize("n", range(40))
def test_writes_without_commit(db, n):
    db.execute("delete from events where id < 1000")
    assert db.execute("select count(*) from events").fetchone()[0] == ROWS - 1000
