"""Seed the database with deterministic test data."""
import random


def seed_database(conn):
    """Create schema and insert deterministic seed data."""
    from shop.schema import SCHEMA

    conn.executescript(SCHEMA)
    rng = random.Random(1234)

    # 200 products with generous stock to handle 3000 orders
    products = []
    for i in range(1, 201):
        sku = f"SKU-{i:04d}"
        name = f"Product {i:04d}"
        price_cents = 100 + (i * 49) % 9900  # 100..9999
        stock = 200 + (i * 137) % 401  # 200..600
        products.append((i, sku, name, price_cents, stock))
    conn.executemany(
        "INSERT INTO products(id, sku, name, price_cents, stock) VALUES(?,?,?,?,?)",
        products,
    )

    # 1000 customers with generous credit
    customers = []
    for i in range(1, 1001):
        name = f"Customer {i:04d}"
        email = f"c{i:04d}@example.com"
        credit_cents = 500000 + 137 * i  # plenty for 3 orders per customer
        customers.append((i, name, email, credit_cents))
    conn.executemany(
        "INSERT INTO customers(id, name, email, credit_cents) VALUES(?,?,?,?)",
        customers,
    )

    prod_by_id = {p[0]: p for p in products}

    # Generate orders: track running stock/credit
    running_stock = {p[0]: p[4] for p in products}
    running_credit = {c[0]: c[3] for c in customers}

    for order_id in range(1, 3001):
        customer_id = ((order_id - 1) % 1000) + 1
        day_offset = (order_id - 1) % 30
        created_at = f"2026-09-{(1 + day_offset):02d}"

        rng_local = random.Random(1234 + order_id)
        num_items = rng_local.randint(1, 3)

        # Build item list deterministically, then validate
        candidate_items = []
        seen_products = set()
        for item_idx in range(num_items):
            prod_id = ((order_id * 7 + item_idx * 13) % 200) + 1
            while prod_id in seen_products:
                prod_id = (prod_id % 200) + 1
            seen_products.add(prod_id)
            desired_qty = rng_local.randint(1, 5)
            unit_price = prod_by_id[prod_id][3]
            # Cap to available stock
            available = running_stock.get(prod_id, 0)
            actual_qty = min(desired_qty, available)
            if actual_qty > 0:
                candidate_items.append((prod_id, actual_qty, unit_price))

        if not candidate_items:
            # Shouldn't happen, but fallback: pick any product with stock
            for pid in range(1, 201):
                if running_stock.get(pid, 0) >= 1:
                    candidate_items.append((pid, 1, prod_by_id[pid][3]))
                    break

        total = sum(qty * price for _, qty, price in candidate_items)

        # If credit insufficient, reduce to cheapest item
        credit = running_credit[customer_id]
        if credit < total:
            # Pick the cheapest item from candidates
            cheapest = min(candidate_items, key=lambda x: x[1] * x[2])
            candidate_items = [cheapest]
            total = cheapest[1] * cheapest[2]

        # Final safety: ensure total <= credit
        assert total <= running_credit[customer_id], (
            f"Order {order_id}: total {total} > credit {running_credit[customer_id]}"
        )

        # Deduct
        for prod_id, qty, _ in candidate_items:
            running_stock[prod_id] -= qty
        running_credit[customer_id] -= total

        conn.execute(
            "INSERT INTO orders(id, customer_id, status, created_at, total_cents) VALUES(?,?,?,?,?)",
            (order_id, customer_id, "placed", created_at, total),
        )
        for prod_id, qty, price in candidate_items:
            conn.execute(
                "INSERT INTO order_items(order_id, product_id, qty, unit_price_cents) VALUES(?,?,?,?)",
                (order_id, prod_id, qty, price),
            )

    # Update actual stock and credit to reflect orders
    for pid, stock_level in running_stock.items():
        conn.execute("UPDATE products SET stock = ? WHERE id = ?", (stock_level, pid))
    for cid, credit in running_credit.items():
        conn.execute("UPDATE customers SET credit_cents = ? WHERE id = ?", (credit, cid))

    conn.commit()
