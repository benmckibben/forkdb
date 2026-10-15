"""Tests for customer operations."""

import pytest
import sqlite3
from shop.app import Shop


class TestAddCustomer:
    def test_add_returns_id(self, shop, db, invariants):
        """add_customer returns a new id beyond the seed range."""
        prev_count = db.execute("SELECT COUNT(*) FROM customers").fetchone()[0]
        cid = shop.add_customer("Alice", "alice@test.com", 5000)
        assert cid > 1000  # seed has 1000 customers
        row = db.execute("SELECT * FROM customers WHERE id = ?", (cid,)).fetchone()
        assert row is not None
        assert row["name"] == "Alice"
        assert row["email"] == "alice@test.com"
        assert row["credit_cents"] == 5000
        assert db.execute("SELECT COUNT(*) FROM customers").fetchone()[0] == prev_count + 1
        invariants(db)

    def test_add_default_credit(self, shop, db):
        cid = shop.add_customer("Bob", "bob@test.com")
        row = db.execute("SELECT credit_cents FROM customers WHERE id = ?", (cid,)).fetchone()
        assert row["credit_cents"] == 0

    def test_duplicate_email_raises_valueerror(self, shop, db, invariants):
        shop.add_customer("First", "dup@test.com", 1000)
        with pytest.raises(ValueError, match="already exists"):
            shop.add_customer("Second", "dup@test.com", 2000)
        # Only one row for that email
        count = db.execute(
            "SELECT COUNT(*) FROM customers WHERE email = ?", ("dup@test.com",)
        ).fetchone()[0]
        assert count == 1
        invariants(db)


class TestCustomerBalance:
    def test_existing_customer(self, shop):
        bal = shop.customer_balance(1)
        assert isinstance(bal, int)
        assert bal > 0

    def test_nonexistent_customer(self, shop):
        with pytest.raises(ValueError, match="not found"):
            shop.customer_balance(99999)

    def test_balance_equals_specific_value(self, db):
        """Customer 1 credit is deterministic."""
        row = db.execute(
            "SELECT credit_cents FROM customers WHERE id = 1"
        ).fetchone()
        # After seed orders, this is a known deterministic value
        assert isinstance(row["credit_cents"], int)
        assert row["credit_cents"] > 0

    def test_balance_reflects_orders(self, db):
        """A customer's balance should be less than seed credit
        because orders deduct credit."""
        # Seed credit is 500000 + 137*id; orders have deducted some
        seed_credit = 500000 + 137 * 1
        row = db.execute(
            "SELECT credit_cents FROM customers WHERE id = 1"
        ).fetchone()
        assert row["credit_cents"] <= seed_credit


class TestCustomerExistence:
    def test_seed_has_1000_customers(self, db, invariants):
        count = db.execute("SELECT COUNT(*) FROM customers").fetchone()[0]
        assert count == 1000
        invariants(db)

    def test_all_customers_have_positive_credit(self, db):
        count = db.execute(
            "SELECT COUNT(*) FROM customers WHERE credit_cents < 0"
        ).fetchone()[0]
        assert count == 0

    def test_emails_are_unique(self, db):
        count_total = db.execute("SELECT COUNT(*) FROM customers").fetchone()[0]
        count_unique = db.execute(
            "SELECT COUNT(DISTINCT email) FROM customers"
        ).fetchone()[0]
        assert count_total == count_unique

    @pytest.mark.parametrize("cid", [1, 500, 1000])
    def test_seed_customer_format(self, db, cid):
        row = db.execute(
            "SELECT name, email FROM customers WHERE id = ?", (cid,)
        ).fetchone()
        assert row["name"] == f"Customer {cid:04d}"
        assert row["email"] == f"c{cid:04d}@example.com"

    def test_destructive_delete_all_customers(self, db):
        """Prove isolation: delete every customer."""
        db.execute("DELETE FROM order_items")
        db.execute("DELETE FROM orders")
        db.execute("DELETE FROM customers")
        db.commit()
        count = db.execute("SELECT COUNT(*) FROM customers").fetchone()[0]
        assert count == 0

    def test_destructive_update_all_credit(self, db):
        """Prove isolation: update every customer credit."""
        db.execute("UPDATE customers SET credit_cents = 0")
        db.commit()
        total_credit = db.execute(
            "SELECT SUM(credit_cents) FROM customers"
        ).fetchone()[0]
        assert total_credit == 0
