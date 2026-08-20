-- Link a website account to a Discord user so buyers get the Pro role automatically.
--
-- There is no way to know which Discord user owns which purchase without them proving it, so
-- the link is established by OAuth rather than by asking people to type their tag.
--
-- Safe to re-run apart from the ALTER, which errors harmlessly if the column already exists.
ALTER TABLE users ADD COLUMN discord_id TEXT;

CREATE INDEX IF NOT EXISTS users_discord ON users (discord_id);

-- Short-lived CSRF state for the OAuth round trip. Without it, an attacker could hand a
-- victim a callback URL carrying the attacker's authorisation code and bind the attacker's
-- Discord account to the victim's licence.
CREATE TABLE IF NOT EXISTS discord_states (
  state_hash TEXT PRIMARY KEY,
  user_id    TEXT NOT NULL,
  expires    INTEGER NOT NULL
);
