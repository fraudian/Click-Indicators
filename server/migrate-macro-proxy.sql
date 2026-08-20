-- Per-device rate limiting for the macro proxy (/api/macros, /api/macro).
--
-- The proxy is what actually gates the product: a build with the licence check
-- patched out still cannot produce a paid device token, so it gets an empty list.
-- This table only stops a token that HAS been shared from being hammered.
--
-- Apply with:
--   wrangler d1 execute clickindicators --remote --file server/migrate-macro-proxy.sql
--
-- rateOK() swallows any error from this table and allows the request, so the
-- proxy keeps working if this migration has not been applied yet. That is
-- deliberate: a missing table must never lock a paying customer out.

CREATE TABLE IF NOT EXISTS macro_rl (
  device_id INTEGER NOT NULL,
  window    INTEGER NOT NULL,   -- unix seconds / 60
  hits      INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (device_id, window)
);

-- Rows age out by window; this index makes the eventual purge cheap.
-- (A scheduled purge is still outstanding - see the week-one list.)
CREATE INDEX IF NOT EXISTS macro_rl_window ON macro_rl (window);
