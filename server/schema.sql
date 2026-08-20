-- Click Indicators accounts + entitlements (Cloudflare D1 / SQLite)
--
-- Model: you buy on the website, then log in inside Geometry Dash with the same
-- email and password. The mod never stores the password - it exchanges it once for
-- a device token, and that token is what gets checked from then on.

CREATE TABLE IF NOT EXISTS users (
  id       TEXT PRIMARY KEY,
  email    TEXT UNIQUE NOT NULL,       -- always stored lowercased
  pw_hash  TEXT NOT NULL,              -- PBKDF2-SHA256, 210k iters, base64
  pw_salt  TEXT NOT NULL,              -- 16 random bytes, base64
  paid     INTEGER NOT NULL DEFAULT 0, -- 1 once a Stripe payment has completed
  created  INTEGER NOT NULL
);

-- every completed purchase, kept for support and refund handling
CREATE TABLE IF NOT EXISTS purchases (
  id             TEXT PRIMARY KEY,     -- Stripe checkout session id
  user_id        TEXT NOT NULL,
  amount         INTEGER,
  currency       TEXT,
  status         TEXT NOT NULL DEFAULT 'paid',  -- paid | refunded
  created        INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS purchases_user ON purchases(user_id);

-- one row per Geometry Dash install that has logged in
CREATE TABLE IF NOT EXISTS devices (
  id         TEXT PRIMARY KEY,
  user_id    TEXT NOT NULL,
  token_hash TEXT UNIQUE NOT NULL,     -- SHA-256 of the token we handed the mod
  name       TEXT,
  created    INTEGER NOT NULL,
  last_seen  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS devices_user ON devices(user_id);

-- website login cookies, stored hashed so a database leak is not a login leak
CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT PRIMARY KEY,
  user_id    TEXT NOT NULL,
  expires    INTEGER NOT NULL
);

-- Stripe event ids already handled, so a retried webhook cannot grant twice
CREATE TABLE IF NOT EXISTS stripe_events (
  id      TEXT PRIMARY KEY,
  created INTEGER NOT NULL
);

-- crude but effective brute-force brake on login
CREATE TABLE IF NOT EXISTS login_attempts (
  ident   TEXT PRIMARY KEY,            -- email or ip
  fails   INTEGER NOT NULL DEFAULT 0,
  until   INTEGER NOT NULL DEFAULT 0   -- locked out until this unix time
);

-- why a device's token stopped working, kept after the device row itself is gone so
-- the mod can explain itself instead of just failing. See migrate-one-device.sql.
CREATE TABLE IF NOT EXISTS device_signouts (
  token_hash TEXT PRIMARY KEY,
  reason     TEXT NOT NULL,            -- 'displaced' | 'removed'
  created    INTEGER NOT NULL
);
