-- Telegram macro source (t.me/gdmacros), added alongside hyperbolus.
--
-- Unlike hyperbolus this cannot be a read-through proxy: Telegram serves no file bytes over HTTP.
-- Every document link in the public preview points at the message page, and the only CDN URLs are
-- photo thumbnails. So the channel is MIRRORED - an out-of-band ingester pulls bytes over MTProto
-- into R2, and this table is the index the Worker serves from. The Telegram credential lives on the
-- ingest host and reaches neither the Worker nor the mod.

CREATE TABLE IF NOT EXISTS tg_macros (
  -- The channel message id of the DOCUMENT album. Natural primary key: one mirrored file per post.
  msg_id     INTEGER PRIMARY KEY,
  level_id   INTEGER NOT NULL,
  format     TEXT    NOT NULL,          -- 'gdr2' | 'slc' - both already parsed by src/parsers.cpp
  r2_key     TEXT    NOT NULL,          -- 'tg/<sha256>.<ext>', content addressed
  bytes      INTEGER NOT NULL,

  filename   TEXT,                      -- as posted, for display only
  level_name TEXT,
  credit     TEXT,                      -- caption "By <name>" - the LEVEL builder, not the botter
  showcase   TEXT,                      -- caption "Showcase: <name>" - a third party again
  fps        REAL,

  -- Decoded from the bytes at ingest, not scraped. inputs = 0 means a structurally valid file with
  -- no actual inputs (EclipseBot writes 60-80 byte .gdr2 stubs); those never reach this table.
  inputs     INTEGER NOT NULL DEFAULT 0,

  -- 1 only when the FILE ITSELF asserted this level id and it matched the caption. .gdr2 embeds a
  -- level id; .slc does not, so ~94% of rows are legitimately 0. Ranked ahead of everything else so
  -- a self-verifying file always beats a caption-only one.
  verified   INTEGER NOT NULL DEFAULT 0,

  -- Default 0 on purpose. Anyone can submit to @GDmacrosBot, so a brand new post must not be able
  -- to become the file that auto-loads on 808 machines the same hour. The backfill is bulk-approved
  -- in one statement after review; incremental rows age in.
  serve      INTEGER NOT NULL DEFAULT 0,

  posted     INTEGER,                   -- unix seconds, from the post
  added      INTEGER NOT NULL           -- unix seconds, when we mirrored it
);

-- The only query the Worker runs: newest servable macros for one level.
CREATE INDEX IF NOT EXISTS tg_macros_level ON tg_macros (level_id, serve, verified DESC, msg_id DESC);

-- Why a post was passed over. Kept because a sudden shift in the reason histogram is the signal
-- that the channel changed its format mix or its caption grammar - which it has done three times.
CREATE TABLE IF NOT EXISTS tg_skipped (
  msg_id INTEGER PRIMARY KEY,
  reason TEXT NOT NULL,
  at     INTEGER NOT NULL
);

-- Ingester cursor, so an interrupted run resumes instead of restarting.
CREATE TABLE IF NOT EXISTS tg_state (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
