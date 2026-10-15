"""Tests for order placement."""

import pytest
import sqlite3
from shop.app import Shop, OutOfStock, InsufficientCredit


class TestPlaceOrderHappyPath:
    def test_place_single_item(self, shop, db, invariants):
        """Place an order for one product and verify state changes."""
        # Get pre-order state
        cid = 1
        pid = 1
        old_credit = db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (cid,)
        ).fetchone()["credit_cents"]
        old_stock = db.execute(
            "SELECT stock FROM products WHERE id = ?", (pid,)
        ).fetchone()["stock"]
        price = db.execute(
            "SELECT price_cents FROM products WHERE id = ?", (pid,)
        ).fetchone()["price_cents"]
        qty = 2

        oid = shop.place_order(cid, [(pid, qty)])
        assert oid > 3000  # seed has 3000 orders
        assert isinstance(oid, int)

        # Verify order row
        row = db.execute("SELECT * FROM orders WHERE id = ?", (oid,)).fetchone()
        assert row is not None
        assert row["customer_id"] == cid
        assert row["status"] == "placed"
        assert row["total_cents"] == price * qty

        # Verify order_items
        item = db.execute(
            "SELECT product_id, qty, unit_price_cents FROM order_items WHERE order_id = ?",
            (oid,),
        ).fetchone()
        assert item["product_id"] == pid
        assert item["qty"] == qty
        assert item["unit_price_cents"] == price

        # Verify stock decremented
        new_stock = db.execute(
            "SELECT stock FROM products WHERE id = ?", (pid,)
        ).fetchone()["stock"]
        assert new_stock == old_stock - qty

        # Verify credit decremented
        new_credit = db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (cid,)
        ).fetchone()["credit_cents"]
        assert new_credit == old_credit - price * qty

        invariants(db)

    def test_place_multiple_items(self, shop, db):
        """Place an order with multiple different products."""
        cid = 2
        items = [(10, 1), (20, 3)]
        old_credits = {}
        for pid, _ in items:
            old_credits[pid] = db.execute(
                "SELECT stock FROM products WHERE id = ?", (pid,)
            ).fetchone()["stock"]

        oid = shop.place_order(cid, items)
        row = db.execute("SELECT total_cents FROM orders WHERE id = ?", (oid,)).fetchone()
        assert row["total_cents"] > 0

        # Check items
        oi_rows = db.execute(
            "SELECT product_id, qty FROM order_items WHERE order_id = ? ORDER BY product_id",
            (oid,),
        ).fetchall()
        assert len(oi_rows) == 2
        assert oi_rows[0]["product_id"] == 10
        assert oi_rows[0]["qty"] == 1
        assert oi_rows[1]["product_id"] == 20
        assert oi_rows[1]["qty"] == 3

    def test_order_with_same_product_twice(self, shop, db):
        """Order the same product twice: app merges qty in a single order_items row."""
        cid = 3
        pid = 15
        qty1, qty2 = 1, 2
        old_stock = db.execute(
            "SELECT stock FROM products WHERE id = ?", (pid,)
        ).fetchone()["stock"]

        oid = shop.place_order(cid, [(pid, qty1), (pid, qty2)])
        items = db.execute(
            "SELECT product_id, qty FROM order_items WHERE order_id = ?",
            (oid,),
        ).fetchall()
        # App merges two entries for same product into one order_items row
        assert len(items) == 1
        assert items[0]["qty"] == qty1 + qty2

        new_stock = db.execute(
            "SELECT stock FROM products WHERE id = ?", (pid,)
        ).fetchone()["stock"]
        assert new_stock == old_stock - qty1 - qty2

    def test_order_uses_app_writes_visible_via_db(self, shop, db):
        """The db fixture sees committed app writes on the same branch."""
        cid = 4
        pid = 30
        oid = shop.place_order(cid, [(pid, 1)])
        # Read via db connection
        assert db.execute(
            "SELECT COUNT(*) FROM orders WHERE id = ?", (oid,)
        ).fetchone()[0] == 1


class TestPlaceOrderErrors:
    def test_out_of_stock(self, shop, db):
        """Request more stock than available."""
        cid = 5
        pid = 50
        max_stock = shop.stock_level(pid)
        old_credit = shop.customer_balance(cid)

        with pytest.raises(OutOfStock):
            shop.place_order(cid, [(pid, max_stock + 1000)])

        # Stock and credit unchanged — verified via shop to ensure rollback worked
        assert shop.stock_level(pid) == max_stock
        assert shop.customer_balance(cid) == old_credit
        # No order row created
        assert db.execute("SELECT MAX(id) FROM orders").fetchone()[0] == 3000

    def test_insufficient_credit(self, shop, db):
        """Order total exceeds customer credit."""
        cid = 6
        credit = db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (cid,)
        ).fetchone()["credit_cents"]
        # Find an expensive product with enough stock to avoid OutOfStock
        # and where qty * price > credit
        rows = db.execute(
            "SELECT id, price_cents, stock FROM products "
            "WHERE stock * price_cents > ? ORDER BY price_cents DESC",
            (credit,),
        ).fetchall()
        assert len(rows) > 0, "No product has enough stock to exceed credit"
        pid = rows[0]["id"]
        price = rows[0]["price_cents"]
        qty = credit // price + 1

        with pytest.raises(InsufficientCredit):
            shop.place_order(cid, [(pid, qty)])

    def test_nonexistent_product(self, shop, db):
        with pytest.raises(ValueError, match="not found"):
            shop.place_order(1, [(99999, 1)])
        # No order created
        assert db.execute("SELECT MAX(id) FROM orders").fetchone()[0] == 3000

    def test_nonexistent_customer(self, shop, db):
        with pytest.raises(ValueError, match="not found"):
            shop.place_order(99999, [(1, 1)])
        assert db.execute("SELECT MAX(id) FROM orders").fetchone()[0] == 3000

    def test_empty_items(self, shop, db):
        with pytest.raises(ValueError, match="at least one item"):
            shop.place_order(1, [])

    def test_zero_quantity(self, shop, db):
        with pytest.raises(ValueError, match="Invalid quantity"):
            shop.place_order(1, [(1, 0)])

    def test_negative_quantity(self, shop, db):
        with pytest.raises(ValueError, match="Invalid quantity"):
            shop.place_order(1, [(1, -1)])


class TestRollbackOnPartialFailure:
    def test_second_item_out_of_stock_rolls_back_everything(self, shop, db):
        """When the second item in an order is out of stock, nothing is committed."""
        cid = 7
        pid1 = 100
        pid2 = 101
        stock1_before = shop.stock_level(pid1)
        stock2_before = shop.stock_level(pid2)
        credit_before = shop.customer_balance(cid)

        with pytest.raises(OutOfStock):
            shop.place_order(cid, [(pid1, 1), (pid2, stock2_before + 1)])

        # Everything unchanged — verified via shop to catch missing rollback
        assert shop.stock_level(pid1) == stock1_before
        assert shop.stock_level(pid2) == stock2_before
        assert shop.customer_balance(cid) == credit_before
        assert db.execute("SELECT MAX(id) FROM orders").fetchone()[0] == 3000

    def test_insufficient_credit_rolls_back_stock(self, shop, db):
        """Credit failure rolls back stock decrements."""
        cid = 8
        credit = shop.customer_balance(cid)
        # Find a product with enough stock that qty * price > credit
        rows = db.execute(
            "SELECT id, price_cents, stock FROM products "
            "WHERE stock * price_cents > ? ORDER BY price_cents DESC",
            (credit,),
        ).fetchall()
        assert len(rows) > 0, "No product has enough stock to exceed credit"
        pid = rows[0]["id"]
        qty = credit // rows[0]["price_cents"] + 1
        stock_before = shop.stock_level(pid)

        with pytest.raises(InsufficientCredit):
            shop.place_order(cid, [(pid, qty)])

        # Verified via shop to catch missing rollback (same connection)
        assert shop.stock_level(pid) == stock_before
        assert db.execute("SELECT MAX(id) FROM orders").fetchone()[0] == 3000

    def test_nonexistent_product_rolls_back_earlier_items(self, shop, db):
        """Product not found in second position: first item stock untouched."""
        cid = 9
        pid1 = 103
        stock_before = shop.stock_level(pid1)

        with pytest.raises(ValueError, match="not found"):
            shop.place_order(cid, [(pid1, 1), (99999, 1)])

        # Verified via shop to catch missing rollback (same connection)
        assert shop.stock_level(pid1) == stock_before
        assert db.execute("SELECT MAX(id) FROM orders").fetchone()[0] == 3000


    def test_connection_clean_after_failure(self, shop, db):
        """After a failed place_order, the same shop can place another order
        on a clean connection (no dangling transaction side-effects).
        This catches M2: missing rollback in the except clause."""
        cid = 10
        pid1 = 110
        pid2 = 111
        stock1_before = shop.stock_level(pid1)
        stock2_before = shop.stock_level(pid2)
        credit_before = shop.customer_balance(cid)
        price1 = db.execute(
            "SELECT price_cents FROM products WHERE id = ?", (pid1,)
        ).fetchone()["price_cents"]

        with pytest.raises(OutOfStock):
            shop.place_order(cid, [(pid1, 1), (pid2, stock2_before + 1)])

        # Now place a successful order on the SAME shop instance
        oid = shop.place_order(cid, [(pid1, 1)])
        # Verify clean state via shop methods (same connection as failed order):
        # only the successful order's deduction is visible
        assert shop.stock_level(pid1) == stock1_before - 1, (
            f"stock1: expected {stock1_before - 1}, got {shop.stock_level(pid1)}"
        )
        assert shop.stock_level(pid2) == stock2_before
        assert shop.customer_balance(cid) == credit_before - price1
        # Verify via db too as a cross-check
        row = db.execute("SELECT * FROM orders WHERE id = ?", (oid,)).fetchone()
        assert row is not None
        assert row["total_cents"] == price1


class TestOrderTotals:
    def test_single_item_total(self, shop, db):
        pid = 55
        price = db.execute(
            "SELECT price_cents FROM products WHERE id = ?", (pid,)
        ).fetchone()["price_cents"]
        oid = shop.place_order(10, [(pid, 3)])
        total = db.execute(
            "SELECT total_cents FROM orders WHERE id = ?", (oid,)
        ).fetchone()["total_cents"]
        assert total == price * 3

    def test_multi_item_total(self, shop, db):
        items = [(60, 1), (61, 2), (62, 1)]
        expected = 0
        for pid, qty in items:
            price = db.execute(
                "SELECT price_cents FROM products WHERE id = ?", (pid,)
            ).fetchone()["price_cents"]
            expected += price * qty
        oid = shop.place_order(11, items)
        total = db.execute(
            "SELECT total_cents FROM orders WHERE id = ?", (oid,)
        ).fetchone()["total_cents"]
        assert total == expected


class TestSeedOrderCount:
    def test_seed_has_3000_orders(self, db, invariants):
        count = db.execute("SELECT COUNT(*) FROM orders").fetchone()[0]
        assert count == 3000
        invariants(db)

    def test_all_seed_orders_are_placed(self, db):
        cancelled = db.execute(
            "SELECT COUNT(*) FROM orders WHERE status != 'placed'"
        ).fetchone()[0]
        assert cancelled == 0


class TestDestructiveOrders:
    def test_delete_all_orders(self, db):
        """Prove isolation: delete every order."""
        db.execute("DELETE FROM order_items")
        db.execute("DELETE FROM orders")
        db.commit()
        assert db.execute("SELECT COUNT(*) FROM orders").fetchone()[0] == 0

    def test_drop_products_table(self, db):
        """Prove isolation: DROP TABLE."""
        db.execute("DROP TABLE IF EXISTS order_items")
        db.execute("DROP TABLE IF EXISTS orders")
        db.execute("DROP TABLE IF EXISTS products")
        db.commit()
        # This should work (no FK violations on empty tables)
        assert True

    def test_fill_with_junk_rows(self, db):
        """Prove isolation: fill database with junk."""
        for i in range(20000):
            db.execute(
                "INSERT INTO customers(name, email, credit_cents) VALUES(?,?,?)",
                (f"Junk {i}", f"junk{i}@test.com", 1),
            )
        db.commit()
        count = db.execute("SELECT COUNT(*) FROM customers").fetchone()[0]
        assert count > 20000

    def test_leave_transaction_open(self, db):
        """Prove isolation: open a transaction and don't close it.

        This must not affect other tests because the branch is discarded.
        """
        db.execute("BEGIN")
        db.execute("DELETE FROM order_items")
        db.execute("DELETE FROM orders")
        # Do NOT commit or rollback — the branch is discarded after this test
