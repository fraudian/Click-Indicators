-- PayPal orders.
--
-- Stripe cannot process PayPal for a US merchant, so PayPal is a direct integration and this
-- table is what binds an order to a buyer. The binding is written when the order is created,
-- server-side, and capture reads the owner from here rather than from the request - so
-- presenting somebody else's order id cannot land a licence on the wrong account.
--
-- amount/currency are recorded at creation and re-checked against what PayPal says actually
-- moved, so an order approved for a different sum than the one quoted is refused.
--
-- Apply with:
--   wrangler d1 execute clickindicators --remote --file server/migrate-paypal.sql

CREATE TABLE IF NOT EXISTS paypal_orders (
  id       TEXT PRIMARY KEY,              -- PayPal order id
  user_id  TEXT NOT NULL,
  amount   INTEGER NOT NULL,              -- minor units, as quoted at creation
  currency TEXT NOT NULL,                 -- uppercase, as quoted at creation
  status   TEXT NOT NULL DEFAULT 'created',  -- created | captured
  capture  TEXT,                          -- PayPal capture id, for matching refunds back
  created  INTEGER NOT NULL
);

-- Refund and dispute webhooks arrive keyed on the capture, not the order.
CREATE INDEX IF NOT EXISTS paypal_orders_capture ON paypal_orders (capture);
CREATE INDEX IF NOT EXISTS paypal_orders_user    ON paypal_orders (user_id);
