"""Not collected by default (the name does not start with test_): run_canaries.sh passes it explicitly. The first test fails on purpose after writing;
the second must still see the untouched seed."""
import pytest

from conftest import SEED_ROWS


def test_a_failing_test_that_wrote_first(db):
    db.execute("delete from t"); db.commit()
    assert False, "on purpose"


def test_b_next_test_sees_the_seed(db):
    assert db.execute("select count(*) from t").fetchone()[0] == SEED_ROWS
