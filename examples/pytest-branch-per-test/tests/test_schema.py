"""Tests for schema: triggers, constraints, foreign keys, view."""

import pytest
import sqlite3


class TestConstraints:
    def test_credit_check_rejects_negative(self, db):
        """credit_cents must be >= 0."""
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "INSERT INTO customers(name, email, credit_cents) VALUES(?,?,?)",
                ("Test", "test@neg.com", -1),
            )

    def test_price_check_rejects_zero(self, db):
        """price_cents must be > 0."""
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "INSERT INTO products(id, sku, name, price_cents, stock) VALUES(?,?,?,?,?)",
                (999, "SKU-ZERO", "Zero Price", 0, 10),
            )

    def test_price_check_rejects_negative(self, db):
        """price_cents must be > 0."""
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "INSERT INTO products(id, sku, name, price_cents, stock) VALUES(?,?,?,?,?)",
                (998, "SKU-NEG", "Neg Price", -5, 10),
            )

    def test_stock_check_rejects_negative(self, db):
        """stock must be >= 0."""
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "INSERT INTO products(id, sku, name, price_cents, stock) VALUES(?,?,?,?,?)",
                (997, "SKU-NEGS", "Neg Stock", 100, -1),
            )

    def test_order_status_check_rejects_invalid(self, db):
        """status must be 'placed' or 'cancelled'."""
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "INSERT INTO orders(id, customer_id, status, created_at, total_cents) VALUES(?,?,?,?,?)",
                (99999, 1, "shipped", "2026-01-01", 1000),
            )

    def test_qty_check_rejects_zero(self, db):
        """qty must be > 0."""
        # Need an order first
        db.execute(
            "INSERT INTO orders(id, customer_id, status, created_at, total_cents) VALUES(?,?,?,?,?)",
            (99998, 1, "placed", "2026-01-01", 1000),
        )
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "INSERT INTO order_items(order_id, product_id, qty, unit_price_cents) VALUES(?,?,?,?)",
                (99998, 1, 0, 100),
            )

    def test_qty_check_rejects_negative(self, db):
        """qty must be > 0."""
        db.execute(
            "INSERT INTO orders(id, customer_id, status, created_at, total_cents) VALUES(?,?,?,?,?)",
            (99997, 1, "placed", "2026-01-01", 1000),
        )
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "INSERT INTO order_items(order_id, product_id, qty, unit_price_cents) VALUES(?,?,?,?)",
                (99997, 1, -1, 100),
            )

    def test_negative_stock_not_allowed_by_update(self, db):
        """Cannot UPDATE stock to negative."""
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "UPDATE products SET stock = -1 WHERE id = 1"
            )

    def test_negative_credit_not_allowed_by_update(self, db):
        """Cannot UPDATE credit_cents to negative."""
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "UPDATE customers SET credit_cents = -1 WHERE id = 1"
            )


class TestForeignKeys:
    def test_order_refs_nonexistent_customer(self, db):
        """FK: orders.customer_id -> customers.id."""
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "INSERT INTO orders(id, customer_id, status, created_at, total_cents) VALUES(?,?,?,?,?)",
                (99996, 99999, "placed", "2026-01-01", 1000),
            )

    def test_order_items_refs_nonexistent_order(self, db):
        """FK: order_items.order_id -> orders.id."""
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "INSERT INTO order_items(order_id, product_id, qty, unit_price_cents) VALUES(?,?,?,?)",
                (99999, 1, 1, 100),
            )

    def test_order_items_refs_nonexistent_product(self, db):
        """FK: order_items.product_id -> products.id."""
        db.execute(
            "INSERT INTO orders(id, customer_id, status, created_at, total_cents) VALUES(?,?,?,?,?)",
            (99995, 1, "placed", "2026-01-01", 1000),
        )
        with pytest.raises(sqlite3.IntegrityError):
            db.execute(
                "INSERT INTO order_items(order_id, product_id, qty, unit_price_cents) VALUES(?,?,?,?)",
                (99995, 99999, 1, 100),
            )

    def test_customer_delete_blocked_by_order(self, db):
        """FK prevents deleting a customer with orders."""
        with pytest.raises(sqlite3.IntegrityError):
            db.execute("DELETE FROM customers WHERE id = 1")

    def test_product_delete_blocked_by_order_items(self, db):
        """FK prevents deleting a product referenced by order_items."""
        with pytest.raises(sqlite3.IntegrityError):
            db.execute("DELETE FROM products WHERE id = 1")


class TestTrigger:
    def test_delete_order_with_items_raises(self, db):
        """The trigger prevents deleting an order that has items."""
        with pytest.raises((sqlite3.IntegrityError, sqlite3.DatabaseError)):
            db.execute("DELETE FROM orders WHERE id = 1")

    def test_delete_order_with_items_state_unchanged(self, db):
        """State unchanged after failed delete attempt."""
        count_before = db.execute("SELECT COUNT(*) FROM orders").fetchone()[0]
        items_before = db.execute(
            "SELECT COUNT(*) FROM order_items WHERE order_id = 1"
        ).fetchone()[0]

        try:
            db.execute("DELETE FROM orders WHERE id = 1")
        except (sqlite3.IntegrityError, sqlite3.DatabaseError):
            pass

        count_after = db.execute("SELECT COUNT(*) FROM orders").fetchone()[0]
        items_after = db.execute(
            "SELECT COUNT(*) FROM order_items WHERE order_id = 1"
        ).fetchone()[0]
        assert count_after == count_before
        assert items_after == items_before

    def test_delete_order_without_items_succeeds(self, db):
        """Deleting an order without items should succeed."""
        db.execute(
            "INSERT INTO orders(id, customer_id, status, created_at, total_cents) VALUES(?,?,?,?,?)",
            (99994, 1, "placed", "2026-01-01", 1000),
        )
        db.execute("DELETE FROM orders WHERE id = 99994")
        db.commit()
        row = db.execute("SELECT id FROM orders WHERE id = 99994").fetchone()
        assert row is None


class TestView:
    def test_revenue_by_day_exists(self, db):
        """The view should be queryable."""
        rows = db.execute(
            "SELECT day, orders, revenue_cents FROM revenue_by_day ORDER BY day"
        ).fetchall()
        assert len(rows) > 0

    def test_revenue_by_day_only_placed(self, db):
        """View should only count 'placed' orders."""
        rows = db.execute(
            "SELECT day, orders, revenue_cents FROM revenue_by_day ORDER BY day"
        ).fetchall()
        for row in rows:
            # Verify counts match direct query for placed orders
            direct = db.execute(
                """
                SELECT COUNT(*), COALESCE(SUM(total_cents), 0)
                FROM orders
                WHERE status = 'placed' AND created_at = ?
                """,
                (row["day"],),
            ).fetchone()
            assert row["orders"] == direct[0], (
                f"Day {row['day']}: view has {row['orders']}, direct has {direct[0]}"
            )
            assert row["revenue_cents"] == direct[1], (
                f"Day {row['day']}: view has {row['revenue_cents']}, direct has {direct[1]}"
            )


class TestIntegrityCheck:
    def test_integrity_check_ok(self, db):
        row = db.execute("PRAGMA integrity_check").fetchone()
        assert row[0] == "ok"

    def test_foreign_key_check_clean(self, db):
        rows = db.execute("PRAGMA foreign_key_check").fetchall()
        assert len(rows) == 0

    def test_order_items_fk_check_clean(self, db):
        """Specific FK check on order_items."""
        db.execute("PRAGMA foreign_keys = ON")
        rows = db.execute("PRAGMA foreign_key_check('order_items')").fetchall()
        assert len(rows) == 0
