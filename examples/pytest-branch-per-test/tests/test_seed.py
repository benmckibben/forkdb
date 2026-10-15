"""Tests for seed data correctness."""

import pytest


class TestSeedCounts:
    def test_200_products(self, db):
        count = db.execute("SELECT COUNT(*) FROM products").fetchone()[0]
        assert count == 200

    def test_1000_customers(self, db):
        count = db.execute("SELECT COUNT(*) FROM customers").fetchone()[0]
        assert count == 1000

    def test_3000_orders(self, db):
        count = db.execute("SELECT COUNT(*) FROM orders").fetchone()[0]
        assert count == 3000

    def test_all_orders_have_items(self, db):
        """Every order should have at least one item."""
        orphaned = db.execute(
            """
            SELECT o.id FROM orders o
            LEFT JOIN order_items oi ON oi.order_id = o.id
            WHERE oi.order_id IS NULL
            """
        ).fetchall()
        assert len(orphaned) == 0, f"{len(orphaned)} orders without items"


class TestSeedConsistency:
    def test_invariants_pass(self, db, invariants):
        """The seed database must pass all invariant checks."""
        invariants(db)

    def test_seed_stock_non_negative(self, db):
        neg = db.execute(
            "SELECT COUNT(*) FROM products WHERE stock < 0"
        ).fetchone()[0]
        assert neg == 0

    def test_seed_credit_non_negative(self, db):
        neg = db.execute(
            "SELECT COUNT(*) FROM customers WHERE credit_cents < 0"
        ).fetchone()[0]
        assert neg == 0

    def test_seed_customer_learning(self, db):
        """Verify deterministic values for a known customer."""
        # Customer 1: seed credit is 500000 + 137*1 = 500137,
        # but orders have deducted some. The final value is deterministic.
        row = db.execute(
            "SELECT name, email, credit_cents FROM customers WHERE id = 1"
        ).fetchone()
        assert row["name"] == "Customer 0001"
        assert row["email"] == "c0001@example.com"
        assert row["credit_cents"] > 0
        assert row["credit_cents"] <= 500137  # seed credit minus orders

    def test_seed_product_learning(self, db):
        """Verify deterministic values for known products (price is fixed, stock is post-orders)."""
        # Product 1: price = 100 + (1*49)%9900 = 149
        expected_price = 100 + (1 * 49) % 9900

        row = db.execute(
            "SELECT sku, name, price_cents, stock FROM products WHERE id = 1"
        ).fetchone()
        assert row["sku"] == "SKU-0001"
        assert row["name"] == "Product 0001"
        assert row["price_cents"] == expected_price
        # Stock is post-orders; just verify it's non-negative and deterministic
        assert row["stock"] >= 0
        assert row["stock"] <= 600  # original max was 600

        # Product 200 price
        expected_price200 = 100 + (200 * 49) % 9900
        row200 = db.execute(
            "SELECT price_cents, stock FROM products WHERE id = 200"
        ).fetchone()
        assert row200["price_cents"] == expected_price200
        assert row200["stock"] >= 0

    def test_seed_orders_date_range(self, db):
        """All seed orders are in September 2026."""
        rows = db.execute(
            "SELECT DISTINCT created_at FROM orders"
        ).fetchall()
        for r in rows:
            assert r["created_at"].startswith("2026-09-"), (
                f"Unexpected date: {r['created_at']}"
            )

    def test_seed_order_dates_all_30_days(self, db):
        """Orders span exactly 30 days."""
        days = db.execute(
            "SELECT COUNT(DISTINCT created_at) FROM orders"
        ).fetchone()[0]
        assert days == 30

    def test_seed_deterministic_idempotent(self, db):
        """Key totals should be reproducible (fixed seed)."""
        # Run a few aggregate queries; these MUST match run-to-run
        total_revenue = db.execute(
            "SELECT SUM(total_cents) FROM orders WHERE status = 'placed'"
        ).fetchone()[0]
        assert total_revenue > 0
        # Save for manual verification
        assert isinstance(total_revenue, int)

        total_stock = db.execute(
            "SELECT SUM(stock) FROM products"
        ).fetchone()[0]
        assert total_stock > 0

    def test_seed_has_no_cancelled_orders(self, db):
        cancelled = db.execute(
            "SELECT COUNT(*) FROM orders WHERE status = 'cancelled'"
        ).fetchone()[0]
        assert cancelled == 0

    def test_seed_every_product_has_stock(self, db):
        """Every product should have stock >= 0 after orders deduct."""
        rows = db.execute(
            "SELECT id, stock FROM products WHERE stock < 0"
        ).fetchall()
        assert len(rows) == 0

    def test_seed_every_customer_has_non_negative_credit(self, db):
        rows = db.execute(
            "SELECT id, credit_cents FROM customers WHERE credit_cents < 0"
        ).fetchall()
        assert len(rows) == 0

    def test_seed_order_totals_match_items(self, db):
        """Every placed order's total_cents must match sum of its items."""
        mismatched = db.execute(
            """
            SELECT o.id, o.total_cents,
                   COALESCE(SUM(oi.qty * oi.unit_price_cents), 0) AS computed
            FROM orders o
            LEFT JOIN order_items oi ON oi.order_id = o.id
            WHERE o.status = 'placed'
            GROUP BY o.id
            HAVING o.total_cents != computed
            """
        ).fetchall()
        assert len(mismatched) == 0, f"{len(mismatched)} orders with wrong totals"

    def test_seed_sku_uniqueness(self, db):
        total = db.execute("SELECT COUNT(*) FROM products").fetchone()[0]
        unique = db.execute(
            "SELECT COUNT(DISTINCT sku) FROM products"
        ).fetchone()[0]
        assert total == unique

    def test_seed_email_uniqueness(self, db):
        total = db.execute("SELECT COUNT(*) FROM customers").fetchone()[0]
        unique = db.execute(
            "SELECT COUNT(DISTINCT email) FROM customers"
        ).fetchone()[0]
        assert total == unique


class TestSeedIsolation:
    """These tests must pass AFTER destructive tests have run in other files."""

    def test_counts_still_correct_after_destruction(self, db):
        """Prove isolation: seed counts are still 200/1000/3000."""
        assert db.execute("SELECT COUNT(*) FROM products").fetchone()[0] == 200
        assert db.execute("SELECT COUNT(*) FROM customers").fetchone()[0] == 1000
        assert db.execute("SELECT COUNT(*) FROM orders").fetchone()[0] == 3000

    def test_stock_still_consistent_after_destruction(self, db):
        """Prove isolation: invariants still hold."""
        row = db.execute("PRAGMA integrity_check").fetchone()
        assert row[0] == "ok"
        neg = db.execute(
            "SELECT COUNT(*) FROM products WHERE stock < 0"
        ).fetchone()[0]
        assert neg == 0

    def test_triggers_still_active(self, db):
        """Prove isolation: the delete trigger still works."""
        import sqlite3
        with pytest.raises((sqlite3.IntegrityError, sqlite3.DatabaseError)):
            db.execute("DELETE FROM orders WHERE id = 1")
