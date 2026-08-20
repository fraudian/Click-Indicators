-- One device at a time (see MAX_DEVICES in worker.js).
--
-- When signing in claims the licence off another machine, that machine's token is
-- already gone by the time it next checks in - there is nothing left to look at to
-- explain why. This records the reason against the dead token so the mod can say
-- "signed in on another device" instead of a blank "signed out", which reads as the
-- mod being broken and turns into a support message.
--
-- Safe to re-run: CREATE TABLE IF NOT EXISTS, no ALTER.
CREATE TABLE IF NOT EXISTS device_signouts (
  token_hash TEXT PRIMARY KEY,
  reason     TEXT NOT NULL,      -- 'displaced' (claimed elsewhere) | 'removed' (on the website)
  created    INTEGER NOT NULL
);

-- Accounts carrying up to five devices from the old rule are deliberately left alone.
-- They collapse to one the next time their owner signs in, rather than everyone being
-- kicked out of a level the moment this deploys.
