"""Shop application: a tiny order system backed by SQLite."""

import sqlite3


class OutOfStock(Exception):
    """Raised when a product does not have enough stock."""


class InsufficientCredit(Exception):
    """Raised when a customer does not have enough credit."""


class Shop:
    """A simple shop backed by SQLite with stock and credit tracking."""

    def __init__(self, database):
        """Open the database.

        database: a path string or a 'file:...' URI.
        """
        self._db = sqlite3.connect(
            database,
            uri=isinstance(database, str) and database.startswith("file:"),
        )
        self._db.row_factory = sqlite3.Row
        self._db.execute("PRAGMA foreign_keys = ON")

    def close(self):
        self._db.close()

    def add_customer(self, name, email, credit_cents=0):
        """Add a customer. Returns the new customer id. Raises ValueError on duplicate email."""
        try:
            cur = self._db.execute(
                "INSERT INTO customers(name, email, credit_cents) VALUES(?,?,?)",
                (name, email, credit_cents),
            )
            self._db.commit()
            return cur.lastrowid
        except sqlite3.IntegrityError as e:
            self._db.rollback()
            if "UNIQUE" in str(e) or "unique" in str(e):
                raise ValueError(f"Email '{email}' already exists") from e
            raise

    def place_order(self, customer_id, items):
        """Place an order atomically.

        items: list of (product_id, qty) tuples.

        Returns the new order id.

        Raises OutOfStock, InsufficientCredit, or ValueError.
        """
        if not items:
            raise ValueError("Order must contain at least one item")

        try:
            # Verify customer exists and get credit
            row = self._db.execute(
                "SELECT credit_cents FROM customers WHERE id = ?", (customer_id,)
            ).fetchone()
            if row is None:
                self._db.rollback()
                raise ValueError(f"Customer {customer_id} not found")
            credit = row["credit_cents"]

            # Verify all products exist and check stock, compute total.
            # Merge duplicate products into one line item.
            merged = {}
            for product_id, qty in items:
                if qty <= 0:
                    self._db.rollback()
                    raise ValueError(f"Invalid quantity {qty} for product {product_id}")
                merged[product_id] = merged.get(product_id, 0) + qty

            total = 0
            line_details = []
            # Eagerly decrement stock during validation so rollback matters.
            for product_id, qty in merged.items():
                row = self._db.execute(
                    "SELECT id, price_cents, stock, sku FROM products WHERE id = ?",
                    (product_id,),
                ).fetchone()
                if row is None:
                    self._db.rollback()
                    raise ValueError(f"Product {product_id} not found")
                if row["stock"] < qty:
                    self._db.rollback()
                    raise OutOfStock(
                        f"Product {product_id} (SKU {row['sku']}): "
                        f"requested {qty}, available {row['stock']}"
                    )
                line_total = qty * row["price_cents"]
                total += line_total
                line_details.append((product_id, qty, row["price_cents"]))
                # Eagerly decrement stock
                self._db.execute(
                    "UPDATE products SET stock = stock - ? WHERE id = ?",
                    (qty, product_id),
                )

            if credit < total:
                self._db.rollback()
                raise InsufficientCredit(
                    f"Customer {customer_id}: need {total}, have {credit}"
                )

            # All checks passed: insert order and items, deduct credit
            import datetime

            self._db.execute(
                "UPDATE customers SET credit_cents = credit_cents - ? WHERE id = ?",
                (total, customer_id),
            )
            created_at = datetime.date.today().isoformat()
            cur = self._db.execute(
                "INSERT INTO orders(customer_id, status, created_at, total_cents) VALUES(?,?,?,?)",
                (customer_id, "placed", created_at, total),
            )
            order_id = cur.lastrowid

            for product_id, qty, price in line_details:
                self._db.execute(
                    "INSERT INTO order_items(order_id, product_id, qty, unit_price_cents) VALUES(?,?,?,?)",
                    (order_id, product_id, qty, price),
                )

            self._db.commit()
            return order_id
        except (OutOfStock, InsufficientCredit, ValueError):
            self._db.rollback()
            raise

    def cancel_order(self, order_id):
        """Cancel a placed order: restock, refund credit, set status 'cancelled'.

        Raises ValueError if the order is not in 'placed' status.
        """
        row = self._db.execute(
            "SELECT id, customer_id, status, total_cents FROM orders WHERE id = ?",
            (order_id,),
        ).fetchone()
        if row is None:
            self._db.rollback()
            raise ValueError(f"Order {order_id} not found")
        if row["status"] != "placed":
            self._db.rollback()
            raise ValueError(
                f"Order {order_id} has status '{row['status']}', cannot cancel"
            )

        items = self._db.execute(
            "SELECT product_id, qty FROM order_items WHERE order_id = ?",
            (order_id,),
        ).fetchall()

        for item in items:
            self._db.execute(
                "UPDATE products SET stock = stock + ? WHERE id = ?",
                (item["qty"], item["product_id"]),
            )

        self._db.execute(
            "UPDATE customers SET credit_cents = credit_cents + ? WHERE id = ?",
            (row["total_cents"], row["customer_id"]),
        )
        self._db.execute(
            "UPDATE orders SET status = 'cancelled' WHERE id = ?", (order_id,)
        )
        self._db.commit()

    def stock_level(self, product_id):
        """Return current stock for a product."""
        row = self._db.execute(
            "SELECT stock FROM products WHERE id = ?", (product_id,)
        ).fetchone()
        if row is None:
            raise ValueError(f"Product {product_id} not found")
        return row["stock"]

    def customer_balance(self, customer_id):
        """Return current credit_cents for a customer."""
        row = self._db.execute(
            "SELECT credit_cents FROM customers WHERE id = ?", (customer_id,)
        ).fetchone()
        if row is None:
            raise ValueError(f"Customer {customer_id} not found")
        return row["credit_cents"]

    def revenue_report(self):
        """Return list of (day, orders, revenue_cents) from the revenue_by_day view."""
        rows = self._db.execute(
            "SELECT day, orders, revenue_cents FROM revenue_by_day ORDER BY day"
        ).fetchall()
        return [(r["day"], r["orders"], r["revenue_cents"]) for r in rows]

    def low_stock(self, threshold):
        """Return list of SKUs for products with stock <= threshold."""
        rows = self._db.execute(
            "SELECT sku FROM products WHERE stock <= ? ORDER BY sku", (threshold,)
        ).fetchall()
        return [r["sku"] for r in rows]

    def top_customers(self, n):
        """Return list of (name, spent_cents) for top n customers by placed-order total."""
        rows = self._db.execute(
            """
            SELECT c.name, SUM(o.total_cents) AS spent_cents
            FROM customers c
            JOIN orders o ON o.customer_id = c.id
            WHERE o.status = 'placed'
            GROUP BY c.id
            ORDER BY spent_cents DESC
            LIMIT ?
            """,
            (n,),
        ).fetchall()
        return [(r["name"], r["spent_cents"]) for r in rows]
