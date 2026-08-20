-- Self-service password reset.
--
-- Until this existed there was no recovery at all: a customer who forgot their password had
-- paid for something they could not open, and the only fix was the owner hand-editing the
-- database. Within an hour of launch that was four people.
--
-- Tokens are stored hashed for the same reason session and device tokens are - a leak of this
-- table must not be a leak of live reset links.
--
-- Safe to re-run.
CREATE TABLE IF NOT EXISTS password_resets (
  token_hash TEXT PRIMARY KEY,
  user_id    TEXT NOT NULL,
  expires    INTEGER NOT NULL,
  created    INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS password_resets_user ON password_resets (user_id);
