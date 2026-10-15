SCHEMA = """
CREATE TABLE IF NOT EXISTS customers (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    email TEXT NOT NULL UNIQUE,
    credit_cents INTEGER NOT NULL CHECK(credit_cents >= 0)
);

CREATE TABLE IF NOT EXISTS products (
    id INTEGER PRIMARY KEY,
    sku TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    price_cents INTEGER NOT NULL CHECK(price_cents > 0),
    stock INTEGER NOT NULL CHECK(stock >= 0)
);

CREATE TABLE IF NOT EXISTS orders (
    id INTEGER PRIMARY KEY,
    customer_id INTEGER NOT NULL REFERENCES customers(id),
    status TEXT NOT NULL CHECK(status IN ('placed', 'cancelled')),
    created_at TEXT NOT NULL,
    total_cents INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS order_items (
    order_id INTEGER NOT NULL REFERENCES orders(id),
    product_id INTEGER NOT NULL REFERENCES products(id),
    qty INTEGER NOT NULL CHECK(qty > 0),
    unit_price_cents INTEGER NOT NULL,
    PRIMARY KEY (order_id, product_id)
);

CREATE INDEX IF NOT EXISTS idx_orders_customer ON orders(customer_id);
CREATE INDEX IF NOT EXISTS idx_order_items_product ON order_items(product_id);

CREATE VIEW IF NOT EXISTS revenue_by_day AS
SELECT created_at AS day,
       COUNT(*) AS orders,
       SUM(total_cents) AS revenue_cents
FROM orders
WHERE status = 'placed'
GROUP BY created_at;

CREATE TRIGGER IF NOT EXISTS prevent_delete_order_with_items
BEFORE DELETE ON orders
FOR EACH ROW
WHEN (EXISTS (SELECT 1 FROM order_items WHERE order_id = OLD.id))
BEGIN
    SELECT RAISE(ABORT, 'Cannot delete order with existing items');
END;
"""
