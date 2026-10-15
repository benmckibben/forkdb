"""Tests for order cancellation."""

import pytest
import sqlite3
from shop.app import Shop


class TestCancelOrder:
    def test_cancel_restores_stock(self, shop, db, invariants):
        """Cancel an order and verify stock is restored exactly."""
        pid = 1
        cid = 1
        stock_before = db.execute(
            "SELECT stock FROM products WHERE id = ?", (pid,)
        ).fetchone()["stock"]
        credit_before = db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (cid,)
        ).fetchone()["credit_cents"]
        price = db.execute(
            "SELECT price_cents FROM products WHERE id = ?", (pid,)
        ).fetchone()["price_cents"]
        oid = shop.place_order(cid, [(pid, 3)])

        stock_after_order = db.execute(
            "SELECT stock FROM products WHERE id = ?", (pid,)
        ).fetchone()["stock"]
        assert stock_after_order == stock_before - 3

        shop.cancel_order(oid)

        # Stock restored
        stock_after_cancel = db.execute(
            "SELECT stock FROM products WHERE id = ?", (pid,)
        ).fetchone()["stock"]
        assert stock_after_cancel == stock_before

        # Credit restored
        credit_after_cancel = db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (cid,)
        ).fetchone()["credit_cents"]
        assert credit_after_cancel == credit_before

        # Status is cancelled
        status = db.execute(
            "SELECT status FROM orders WHERE id = ?", (oid,)
        ).fetchone()["status"]
        assert status == "cancelled"

        invariants(db)

    def test_cancel_multi_item_order_restocks_all(self, shop, db):
        """Cancel multi-item order: all products restocked."""
        cid = 2
        items = [(10, 2), (20, 1)]
        stocks_before = {}
        for pid, _ in items:
            stocks_before[pid] = db.execute(
                "SELECT stock FROM products WHERE id = ?", (pid,)
            ).fetchone()["stock"]
        credit_before = db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (cid,)
        ).fetchone()["credit_cents"]

        oid = shop.place_order(cid, items)
        total = db.execute(
            "SELECT total_cents FROM orders WHERE id = ?", (oid,)
        ).fetchone()["total_cents"]

        shop.cancel_order(oid)

        for pid, qty in items:
            stock = db.execute(
                "SELECT stock FROM products WHERE id = ?", (pid,)
            ).fetchone()["stock"]
            assert stock == stocks_before[pid], f"Product {pid} stock not restored"

        credit = db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (cid,)
        ).fetchone()["credit_cents"]
        assert credit == credit_before

    def test_cancel_cancelled_order_raises(self, shop, db):
        """Cannot cancel an already-cancelled order."""
        pid = 3
        cid = 3
        oid = shop.place_order(cid, [(pid, 1)])
        shop.cancel_order(oid)
        with pytest.raises(ValueError, match="cannot cancel"):
            shop.cancel_order(oid)

    def test_cancel_nonexistent_order_raises(self, shop):
        with pytest.raises(ValueError, match="not found"):
            shop.cancel_order(99999)

    def test_cancel_seed_order(self, shop, db, invariants):
        """Cancel a seed order (id 1)."""
        # Get pre-cancel state
        row = db.execute(
            """
            SELECT o.id, o.customer_id, o.total_cents, o.status,
                   c.credit_cents
            FROM orders o
            JOIN customers c ON c.id = o.customer_id
            WHERE o.id = 1
            """
        ).fetchone()
        assert row["status"] == "placed"
        credit_before = row["credit_cents"]
        total = row["total_cents"]

        # Get stock for each item
        items = db.execute(
            "SELECT product_id, qty FROM order_items WHERE order_id = 1"
        ).fetchall()
        stocks_before = {}
        for item in items:
            stocks_before[item["product_id"]] = db.execute(
                "SELECT stock FROM products WHERE id = ?", (item["product_id"],)
            ).fetchone()["stock"]

        shop.cancel_order(1)

        for item in items:
            stock = db.execute(
                "SELECT stock FROM products WHERE id = ?", (item["product_id"],)
            ).fetchone()["stock"]
            assert stock == stocks_before[item["product_id"]] + item["qty"], (
                f"Product {item['product_id']} stock: expected "
                f"{stocks_before[item['product_id']] + item['qty']}, got {stock}"
            )

        credit_after = db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (row["customer_id"],)
        ).fetchone()["credit_cents"]
        assert credit_after == credit_before + total

        status = db.execute(
            "SELECT status FROM orders WHERE id = 1"
        ).fetchone()["status"]
        assert status == "cancelled"

        invariants(db)

    @pytest.mark.parametrize("oid", [100, 500, 1000, 2000, 2999])
    def test_cancel_many_seed_orders(self, shop, db, invariants, oid):
        """Cancel a batch of seed orders parametrized."""
        row = db.execute(
            "SELECT status FROM orders WHERE id = ?", (oid,)
        ).fetchone()
        assert row["status"] == "placed"
        shop.cancel_order(oid)
        status = db.execute(
            "SELECT status FROM orders WHERE id = ?", (oid,)
        ).fetchone()["status"]
        assert status == "cancelled"
        invariants(db)

    def test_cancel_then_reorder_same_items(self, shop, db):
        """After cancel, the same items can be ordered again."""
        cid = 4
        pid = 50
        stock_original = db.execute(
            "SELECT stock FROM products WHERE id = ?", (pid,)
        ).fetchone()["stock"]
        credit_original = db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (cid,)
        ).fetchone()["credit_cents"]

        oid1 = shop.place_order(cid, [(pid, 2)])
        shop.cancel_order(oid1)

        # Stock and credit should be back to original
        stock_mid = db.execute(
            "SELECT stock FROM products WHERE id = ?", (pid,)
        ).fetchone()["stock"]
        credit_mid = db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (cid,)
        ).fetchone()["credit_cents"]
        assert stock_mid == stock_original
        assert credit_mid == credit_original

        # Re-order same items
        oid2 = shop.place_order(cid, [(pid, 2)])
        stock_after2 = db.execute(
            "SELECT stock FROM products WHERE id = ?", (pid,)
        ).fetchone()["stock"]
        assert stock_after2 == stock_original - 2
