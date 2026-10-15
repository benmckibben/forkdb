"""Tests for reports: revenue, low_stock, top_customers."""

import pytest
from shop.app import Shop


class TestRevenueReport:
    def test_revenue_report_returns_list(self, shop):
        report = shop.revenue_report()
        assert isinstance(report, list)

    def test_revenue_report_30_days(self, shop):
        """Seed places orders across 30 days."""
        report = shop.revenue_report()
        # 3000 orders across 30 days, some days might be empty
        assert len(report) <= 30
        for day, orders, revenue in report:
            assert day.startswith("2026-09-")
            assert orders > 0
            assert revenue > 0

    def test_revenue_report_ordered_by_day(self, shop):
        report = shop.revenue_report()
        days = [r[0] for r in report]
        assert days == sorted(days)

    def test_revenue_equals_sum_of_order_totals(self, shop, db):
        """Revenue from report equals direct SUM of placed orders grouped by day."""
        report = shop.revenue_report()
        direct = db.execute(
            """
            SELECT created_at AS day, COUNT(*) AS orders,
                   SUM(total_cents) AS revenue_cents
            FROM orders
            WHERE status = 'placed'
            GROUP BY created_at
            ORDER BY day
            """
        ).fetchall()
        assert len(report) == len(direct)
        for (r_day, r_orders, r_rev), d_row in zip(report, direct):
            assert r_day == d_row["day"]
            assert r_orders == d_row["orders"]
            assert r_rev == d_row["revenue_cents"]

    def test_cancelled_orders_not_in_revenue(self, shop, db):
        """Cancel a few orders and verify they drop out of revenue."""
        db.execute("UPDATE orders SET status = 'cancelled' WHERE id IN (1, 2, 3)")
        db.commit()
        report = shop.revenue_report()
        for day, orders, revenue in report:
            # Cancelled orders on those days should be excluded
            direct = db.execute(
                """
                SELECT COUNT(*), COALESCE(SUM(total_cents), 0)
                FROM orders
                WHERE status = 'placed' AND created_at = ?
                """,
                (day,),
            ).fetchone()
            assert orders == direct[0]
            assert revenue == direct[1]

    def test_new_order_appears_in_revenue(self, shop, db):
        """An order placed today should appear in the report."""
        pid = 1
        cid = 1
        oid = shop.place_order(cid, [(pid, 1)])
        total = db.execute(
            "SELECT total_cents, created_at FROM orders WHERE id = ?", (oid,)
        ).fetchone()

        report = shop.revenue_report()
        found = False
        for day, orders, revenue in report:
            if day == total["created_at"]:
                found = True
                assert orders >= 1
                assert revenue >= total["total_cents"]
        assert found, "New order's day not in revenue report"


class TestLowStock:
    def test_low_stock_returns_list(self, shop):
        result = shop.low_stock(10)
        assert isinstance(result, list)

    def test_low_stock_includes_below_threshold(self, shop, db):
        """Products with stock <= threshold are included."""
        threshold = 20
        result = shop.low_stock(threshold)
        direct = db.execute(
            "SELECT sku FROM products WHERE stock <= ? ORDER BY sku",
            (threshold,),
        ).fetchall()
        assert result == [r["sku"] for r in direct]

    def test_low_stock_excludes_above_threshold(self, shop, db):
        """Products with stock > threshold are excluded."""
        threshold = 0
        result = shop.low_stock(threshold)
        direct = db.execute(
            "SELECT sku FROM products WHERE stock <= 0 ORDER BY sku"
        ).fetchall()
        assert result == [r["sku"] for r in direct]

    def test_low_stock_all_products_with_high_threshold(self, shop, db):
        """A very high threshold should return all 200 products."""
        result = shop.low_stock(1000000)
        assert len(result) == 200

    def test_low_stock_after_buy(self, shop, db):
        """Buying stock changes low_stock results."""
        # Find a cheap product where (stock-1)*price fits within customer 1's credit
        cid = 1
        credit = db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (cid,)
        ).fetchone()["credit_cents"]
        rows = db.execute(
            "SELECT id, price_cents, stock FROM products "
            "WHERE (stock - 1) * price_cents <= ? AND stock > 1 "
            "ORDER BY stock DESC"
            "",
            (credit,),
        ).fetchall()
        assert len(rows) > 0, "No product fits within credit"
        pid = rows[0]["id"]
        stock_before = rows[0]["stock"]
        # Buy all but 1
        shop.place_order(cid, [(pid, stock_before - 1)])
        result = shop.low_stock(2)
        # Product should now have stock == 1, which is <= 2
        sku = db.execute(
            "SELECT sku FROM products WHERE id = ?", (pid,)
        ).fetchone()["sku"]
        assert sku in result


class TestTopCustomers:
    def test_top_customers_returns_list(self, shop):
        result = shop.top_customers(5)
        assert isinstance(result, list)
        assert len(result) == 5

    def test_top_customers_ordered_descending(self, shop):
        result = shop.top_customers(10)
        spent = [s for _, s in result]
        assert spent == sorted(spent, reverse=True)

    def test_top_customers_uses_only_placed_orders(self, shop, db):
        """Cancelled orders should not count toward top_customers."""
        # Cancel all orders for customer 1
        db.execute(
            "UPDATE orders SET status = 'cancelled' WHERE customer_id = 1"
        )
        db.commit()
        result = shop.top_customers(10)
        for name, spent in result:
            assert name != "Customer 0001"

    def test_top_1_has_highest_spend(self, shop, db):
        """Top 1 customer should match the max sum from a direct query."""
        result = shop.top_customers(1)
        assert len(result) == 1
        name, spent = result[0]
        direct = db.execute(
            """
            SELECT c.name, SUM(o.total_cents) AS spent_cents
            FROM customers c
            JOIN orders o ON o.customer_id = c.id
            WHERE o.status = 'placed'
            GROUP BY c.id
            ORDER BY spent_cents DESC
            LIMIT 1
            """
        ).fetchone()
        assert name == direct["name"]
        assert spent == direct["spent_cents"]

    @pytest.mark.parametrize("n", [1, 5, 10, 20, 50])
    def test_top_n_length(self, shop, n):
        result = shop.top_customers(n)
        assert len(result) == n

    def test_top_all_customers(self, shop):
        """top_customers(1000) should return all customers (who have orders)."""
        result = shop.top_customers(1000)
        assert len(result) == 1000

    def test_cancelled_order_does_not_increase_spent(self, shop, db):
        """Place then cancel an order: top_customers spent must stay the same.
        Kills M8: cancelling should not inflate a customer's spent_total."""
        cid = 1
        pid = 1  # SKU-0001, price=149, stock=250
        price = db.execute(
            "SELECT price_cents FROM products WHERE id = ?", (pid,)
        ).fetchone()["price_cents"]

        # Seed spent for customer 1 from the revenue_by_day view (placed only)
        seed_spent_before = db.execute(
            """
            SELECT COALESCE(SUM(o.total_cents), 0)
            FROM orders o
            WHERE o.customer_id = ? AND o.status = 'placed'
            """,
            (cid,),
        ).fetchone()[0]

        # Verify this matches top_customers before we change anything
        all_custs = shop.top_customers(1000)
        spent_before = [s for name, s in all_custs if name == "Customer 0001"][0]
        assert spent_before == seed_spent_before

        # Place an order and cancel it
        oid = shop.place_order(cid, [(pid, 1)])
        shop.cancel_order(oid)

        # Now top_customers must report the SAME spent_cents
        all_custs = shop.top_customers(1000)
        spent_after = [s for name, s in all_custs if name == "Customer 0001"][0]
        assert spent_after == spent_before, (
            f"Customer 1: spent_before={spent_before}, spent_after={spent_after}"
        )

    def test_cancelled_orders_never_increase_spend_for_any_customer(self, shop, db):
        """Cancel one of customer 1's seed orders, place+cancel a new one:
        total spent must only reflect the remaining placed seed orders.
        Kills M8: cancelled-only orders contribute nothing."""
        cid = 1
        pid = 1  # SKU-0001, price=149

        # Cancel one seed order for customer 1 (e.g., order id 1)
        db.execute("UPDATE orders SET status = 'cancelled' WHERE id = 1")
        db.commit()

        # Compute expected spent: seed total minus the cancelled order's total
        cancelled_total = db.execute(
            "SELECT total_cents FROM orders WHERE id = 1"
        ).fetchone()["total_cents"]
        seed_spent = db.execute(
            """
            SELECT COALESCE(SUM(total_cents), 0)
            FROM orders WHERE customer_id = ? AND status = 'placed'
            """,
            (cid,),
        ).fetchone()[0]

        # Place and cancel a new order
        oid = shop.place_order(cid, [(pid, 1)])
        shop.cancel_order(oid)

        # After place+cancel, spent must equal the seed's remaining placed-total
        # (the new order was cancelled, so it doesn't add anything)
        all_custs = shop.top_customers(1000)
        spent = [s for name, s in all_custs if name == "Customer 0001"][0]
        assert spent == seed_spent, (
            f"Customer 1: expected spent={seed_spent}, got={spent}"
        )

    def test_place_cancel_no_effect_on_rank_order(self, shop, db):
        """Place+ cancel for customer 2; verify top_customers rank unchanged.
        Kills M8: the top spot should not change from a cancelled transaction."""
        # Find the current top customer and their spent
        top_before = shop.top_customers(1)
        top_name_before, top_spent_before = top_before[0]

        cid2 = 2
        # Place and immediately cancel for customer 2
        pid = 1  # cheap product (149 cents)
        oid = shop.place_order(cid2, [(pid, 1)])
        shop.cancel_order(oid)

        top_after = shop.top_customers(1)
        top_name_after, top_spent_after = top_after[0]
        # Top customer and their spent must be unchanged
        assert top_name_after == top_name_before, (
            f"Top customer changed: {top_name_before} -> {top_name_after}"
        )
        assert top_spent_after == top_spent_before, (
            f"Top spent changed: {top_spent_before} -> {top_spent_after}"
        )
