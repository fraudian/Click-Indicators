ALTER TABLE users ADD COLUMN verified INTEGER NOT NULL DEFAULT 0;
-- existing accounts predate verification, so grandfather them in
UPDATE users SET verified = 1;

CREATE TABLE IF NOT EXISTS email_codes (
  email     TEXT PRIMARY KEY,
  code_hash TEXT NOT NULL,      -- hashed, so the database never holds a live code
  expires   INTEGER NOT NULL,
  tries     INTEGER NOT NULL DEFAULT 0,
  sent      INTEGER NOT NULL
);
