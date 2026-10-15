"""Shop test fixtures. The database fixtures (db, db_uri) come from the forkdb_pytest plugin (registered in ../conftest.py): this file only says what the
seed is, how connections are set up, and adds the app and invariant fixtures."""

import sqlite3

import pytest

from shop.app import Shop
from shop.seed import seed_database


@pytest.fixture(scope="session")
def forkdb_seed():
    return seed_database


@pytest.fixture(scope="session")
def forkdb_configure():
    def configure(conn):
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys = ON")
    return configure


@pytest.fixture
def shop(db_uri):
    """A Shop instance connected to this test's database branch."""
    s = Shop(db_uri)
    yield s
    s.close()


@pytest.fixture
def invariants():
    """Return a callable(conn) that asserts database consistency."""

    def check(conn):
        row = conn.execute("PRAGMA integrity_check").fetchone()
        assert row[0] == "ok", f"integrity_check: {row[0]}"

        fk_rows = conn.execute("PRAGMA foreign_key_check").fetchall()
        assert len(fk_rows) == 0, f"foreign_key_check: {fk_rows}"

        neg = conn.execute("SELECT COUNT(*) FROM products WHERE stock < 0").fetchone()[0]
        assert neg == 0, f"{neg} products with negative stock"

        neg = conn.execute("SELECT COUNT(*) FROM customers WHERE credit_cents < 0").fetchone()[0]
        assert neg == 0, f"{neg} customers with negative credit"

        order_count = conn.execute("SELECT COUNT(*) FROM orders").fetchone()[0]
        distinct_oi = conn.execute(
            "SELECT COUNT(DISTINCT order_id) FROM order_items"
        ).fetchone()[0]
        assert order_count == distinct_oi, (
            f"order count {order_count} != distinct order_items.order_id {distinct_oi}"
        )

        rows = conn.execute(
            """
            SELECT o.id, o.total_cents,
                   COALESCE(SUM(oi.qty * oi.unit_price_cents), 0) AS computed
            FROM orders o
            LEFT JOIN order_items oi ON oi.order_id = o.id
            WHERE o.status = 'placed'
            GROUP BY o.id
            """
        ).fetchall()
        for r in rows:
            assert r["total_cents"] == r["computed"], (
                f"Order {r['id']}: total_cents={r['total_cents']} "
                f"!= sum={r['computed']}"
            )

    return check
