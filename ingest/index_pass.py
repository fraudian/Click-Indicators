#!/usr/bin/env python3
"""
Stage 1 of the t.me/gdmacros mirror: build the index. No Telegram account, no credential.

Telegram's public channel preview (t.me/s/<channel>) renders posts as HTML and is fetchable by
anyone. It gives us everything EXCEPT the file bytes: message ids, captions, level ids, filenames
and exact sizes. That is enough to decide what is worth downloading before spending a single
authenticated request, which is why this stage exists separately from mirror_telegram.py.

Two things about the channel's shape that this parser exists to handle:

  * ONE POST IS TWO BUBBLES. The caption lives on a photo message at id N; the five documents are
    rendered as a separate bubble at N+1 with EMPTY text. Reading the text off the bubble that
    holds the files gets you an empty caption 100% of the time.

  * THE FORMAT MIX CHANGED THREE TIMES. .gdr2 only appears from 2026-06-27, .cml replaced .ybot on
    2026-05-31, .gdr became .gdr.json around 2026-05. Anything hard-coded to "the current five
    formats" silently misses most of the back catalogue, so we take the best SUPPORTED format
    present on each post and record why we passed on the rest.

Usage:
    python index_pass.py                 # resume, or start from the newest post
    python index_pass.py --full          # walk the whole channel from the top
    python index_pass.py --pages 20      # stop after N pages (for a quick look)
"""

import argparse
import html
import os
import re
import sqlite3
import sys
import time
import urllib.request

CHANNEL = "gdmacros"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126 Safari/537.36"
DB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "work.db")

# Formats src/parsers.cpp can actually read, best first. .gdr2 is ~580 bytes and carries the level
# id inside the file so the caption can be checked against it; .slc is ~2.3 KB and is the only
# format present across nearly the whole archive. Everything else on a post is either unparsable
# (.tcm, .cml, .ybot) or megabytes of per-frame physics state we have no use for (.gdr.json).
# .gdr is third rather than absent: it is bulky (median ~400 KB, because writers attach per-frame
# state our reader then filters out) but for ~234 posts in the 2024-2025 era it is the ONLY format
# present that we can read at all. Taking it turns those from no macro into a working macro.
PREFER = ["gdr2", "slc", "gdr"]

# Hard ceilings, applied to the size the PREVIEW reports, before anything is downloaded. The
# channel really does contain a 620 MB .gdr.json, a 547 MB .gdr, and once listed a 974 MB .slc.
SIZE_CAP = {"gdr2": 256 * 1024, "slc": 1536 * 1024, "gdr": 8 << 20}

SIZE_RE = re.compile(r"([\d.]+)\s*(B|KB|MB|GB)\b", re.I)
UNIT = {"b": 1, "kb": 1024, "mb": 1024 ** 2, "gb": 1024 ** 3}


def to_bytes(s):
    m = SIZE_RE.search(s or "")
    if not m:
        return -1
    return int(float(m.group(1)) * UNIT[m.group(2).lower()])


def fetch(url, tries=4):
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=45) as r:
                return r.read().decode("utf-8", "replace")
        except Exception as e:                      # noqa: BLE001 - transient network, retry
            last = e
            time.sleep(2 * (i + 1))
    print("  ! fetch failed: %s (%s)" % (url, last))
    return None


TAG_RE = re.compile(r"<[^>]+>")
BR_RE = re.compile(r"<br\s*/?>", re.I)


def inner_text(frag):
    """Text of an HTML fragment, with <br> as newline. Entities decoded last so that a caption
    containing a literal '&lt;' cannot smuggle markup back in."""
    if not frag:
        return ""
    return html.unescape(TAG_RE.sub("", BR_RE.sub("\n", frag))).strip()


BUBBLE_RE = re.compile(r'<div class="tgme_widget_message[ "][^>]*?data-post="[^/]+/(\d+)"(.*?)(?=<div class="tgme_widget_message[ "][^>]*?data-post=|\Z)', re.S)
TEXT_RE = re.compile(r'<div class="tgme_widget_message_text[^"]*"[^>]*>(.*?)</div>\s*(?:<div class="tgme_widget_message_(?:footer|reply_markup)|<a class="tgme_widget_message_link_preview)', re.S)
TEXT_FALLBACK_RE = re.compile(r'<div class="tgme_widget_message_text[^"]*"[^>]*>(.*?)</div>', re.S)
# The href matters as much as the filename. An album of five documents renders as ONE bubble whose
# data-post is the FIRST document's message id - so for a post whose files are 15639..15643, the
# bubble says 15639 while the .gdr2 we actually want is 15642. Downloading by bubble id fetches the
# 5.7 MB .gdr.json every time. Only the per-document href carries the real id.
DOC_RE = re.compile(
    r'<a class="tgme_widget_message_document_wrap" href="https://t\.me/[^/"]+/(\d+)[^"]*"[^>]*>'
    r'(?:(?!</a>).)*?tgme_widget_message_document_title[^>]*>(.*?)</div>\s*'
    r'<div class="tgme_widget_message_document_extra"[^>]*>(.*?)</div>', re.S)
TIME_RE = re.compile(r'<time[^>]+datetime="([^"]+)"')

# The numeric id is wrapped in <code> on 100% of current-era posts. Matching the coded form first
# is what keeps us off the "ID:" lines that Telegram inlines from linked YouTube descriptions -
# one real post carries two different "ID: ..." lines, only one of which is the level.
ID_CODE_RE = re.compile(r"ID:\s*<code>(\d{2,12})</code>", re.I)
ID_TEXT_RE = re.compile(r"^[ \t]*I[Dd][ \t]*:?[ \t]*(\d{2,12})[ \t]*(?:\(|$)", re.M)
HASHTAG_RE = re.compile(r"^\s*#(\w+)", re.M)
BY_RE = re.compile(r"\s+[Bb]y\s+")
SHOWCASE_RE = re.compile(r"^\s*Showcase:\s*(.+)$", re.M | re.I)


def parse_caption(text_html):
    """Level id, name, credit and showcase out of one caption. Returns None if there is no
    usable level id - a real and expected outcome: the channel posts 'ID: notreleasedyet',
    'ID: WaitForRelease:D' and 'ID: Unreleased' for levels that are not out yet."""
    out = {"level_id": 0, "name": "", "credit": "", "showcase": "", "difficulty": ""}

    m = ID_CODE_RE.search(text_html or "")
    text = inner_text(text_html)
    if m:
        out["level_id"] = int(m.group(1))
    else:
        m2 = ID_TEXT_RE.search(text)
        if not m2:
            return None
        out["level_id"] = int(m2.group(1))
    # A GD level id is 7-9 digits in practice; anything shorter is almost certainly a song id or a
    # position number that survived the scoping rules.
    if not (10 <= out["level_id"] <= 999999999999):
        return None

    lines = [l.strip() for l in text.split("\n") if l.strip()]
    h = HASHTAG_RE.search(text)
    if h:
        out["difficulty"] = h.group(1)

    # The name line is the one after the difficulty hashtag, or the first line if there is none.
    name_line = ""
    for i, l in enumerate(lines):
        if l.startswith("#"):
            if i + 1 < len(lines):
                name_line = lines[i + 1]
            break
    if not name_line and lines:
        name_line = lines[0]

    # Split on the LAST ' By ' so a level actually called "Stand By Me" keeps its name. (split()
    # returns the FIRST separator's pieces, which would truncate exactly that case.)
    hits = list(BY_RE.finditer(name_line))
    if hits:
        last = hits[-1]
        out["name"] = name_line[:last.start()].strip()
        out["credit"] = name_line[last.end():].strip()
    else:
        out["name"] = name_line.strip()

    s = SHOWCASE_RE.search(text)
    if s:
        out["showcase"] = s.group(1).strip()
    return out


def db_open():
    c = sqlite3.connect(DB)
    c.executescript("""
      CREATE TABLE IF NOT EXISTS posts (
        -- The album bubble id. This is the stable identity of the POST and becomes tg_macros.msg_id
        -- in D1, which is what the kill switch and the /api/macro?t= route key on.
        msg_id     INTEGER PRIMARY KEY,
        -- The message id of the ONE file we mirror, which is NOT msg_id: an album of five documents
        -- occupies five consecutive ids and the bubble reports only the first. Download by this.
        doc_msg_id INTEGER NOT NULL,
        caption_id INTEGER,
        level_id   INTEGER NOT NULL,
        name       TEXT, credit TEXT, showcase TEXT, difficulty TEXT,
        format     TEXT NOT NULL,
        filename   TEXT NOT NULL,
        bytes      INTEGER NOT NULL,
        posted     INTEGER,
        state      TEXT NOT NULL DEFAULT 'indexed',  -- indexed -> mirrored -> synced
        sha256     TEXT, local TEXT, fps REAL, inputs INTEGER, verified INTEGER DEFAULT 0
      );
      CREATE TABLE IF NOT EXISTS skipped (
        msg_id INTEGER PRIMARY KEY, reason TEXT NOT NULL, at INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT NOT NULL);
    """)
    # A work.db written before doc_msg_id existed holds the album bubble id where the file id
    # belongs, which would mirror the wrong document for every row. Nothing here is expensive to
    # rebuild - the index pass needs no credential - so rebuild rather than carry bad ids forward.
    cols = [r[1] for r in c.execute("PRAGMA table_info(posts)")]
    if cols and "doc_msg_id" not in cols:
        print("work.db predates the per-document id fix - re-indexing from scratch\n")
        c.executescript("DROP TABLE posts; DELETE FROM skipped; DELETE FROM state;")
        return db_open()
    return c


def skip(c, msg_id, reason):
    c.execute("INSERT OR REPLACE INTO skipped (msg_id, reason, at) VALUES (?,?,?)",
              (msg_id, reason, int(time.time())))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true", help="walk the whole channel back to the start")
    ap.add_argument("--resume", action="store_true",
                    help="with --full, continue from where the last backfill stopped")
    ap.add_argument("--pages", type=int, default=0, help="stop after N pages")
    args = ap.parse_args()

    c = db_open()

    # The preview only pages BACKWARDS (?before=), so "resume from the cursor" is the wrong shape
    # for picking up new posts - the cursor points at the OLDEST thing seen, and new posts are at
    # the other end. An incremental run therefore always starts at the top and stops once it has
    # seen enough consecutive already-known posts to be sure it has caught up.
    before = None
    if args.full and args.resume:
        cur = c.execute("SELECT value FROM state WHERE key='oldest_seen'").fetchone()
        if cur:
            before = int(cur[0])
    dry_pages = 0

    pages = 0
    added = skipped = 0
    seen_ids = set()
    stall = 0
    # A post's caption sits at a LOWER message id than its documents, and we page backwards - so
    # the caption for the last post on a page is on the NEXT page. Carry both across the boundary
    # or roughly a quarter of all posts get dropped as "no-caption".
    captions = {}
    pending = []

    while True:
        url = "https://t.me/s/%s" % CHANNEL
        if before:
            url += "?before=%d" % before
        page = fetch(url)
        if page is None:
            break

        bubbles = BUBBLE_RE.findall(page)
        if not bubbles:
            print("  no posts on page - end of channel or markup changed")
            break

        # Caption bubbles indexed by id so a document bubble can find its own caption.
        docs = list(pending)
        pending = []
        page_new = 0
        for mid_s, frag in bubbles:
            mid = int(mid_s)
            tm = TEXT_RE.search(frag) or TEXT_FALLBACK_RE.search(frag)
            text_html = tm.group(1) if tm else ""
            d = DOC_RE.findall(frag)
            t = TIME_RE.search(frag)
            posted = 0
            if t:
                try:
                    posted = int(time.mktime(time.strptime(t.group(1)[:19], "%Y-%m-%dT%H:%M:%S")))
                except Exception:                   # noqa: BLE001 - tz forms vary, not worth failing on
                    posted = 0
            if text_html.strip():
                captions[mid] = text_html
            if d:
                docs.append((mid, d, posted))

        lowest = min(int(m) for m, _ in bubbles)

        for mid, d, posted in docs:
            if mid in seen_ids:
                continue
            if c.execute("SELECT 1 FROM posts WHERE msg_id=?", (mid,)).fetchone():
                seen_ids.add(mid)
                continue

            # The caption sits on the bubble before this one. Search back a few ids because old
            # eras sometimes put an empty media bubble in between.
            cap_html, cap_id = "", 0
            for back in (1, 2, 3, 4, 5):
                if mid - back in captions:
                    cap_html, cap_id = captions[mid - back], mid - back
                    break
            if not cap_html:
                # Its caption is probably on the next page (older ids). Hold it there rather than
                # discarding it; only give up once we have seen past where the caption could be.
                if mid - 5 < lowest:
                    pending.append((mid, d, posted))
                    continue
                seen_ids.add(mid)
                skip(c, mid, "no-caption"); skipped += 1; continue
            seen_ids.add(mid)

            info = parse_caption(cap_html)
            if not info:
                skip(c, mid, "no-level-id"); skipped += 1; continue

            files = {}
            for did, title, extra in d:
                fn = inner_text(title)
                ext = fn.rsplit(".", 1)[-1].lower() if "." in fn else ""
                files[ext] = (fn, to_bytes(inner_text(extra)), int(did))

            pick = None
            rejected = []
            for fmt in PREFER:
                if fmt in files:
                    fn, size, did = files[fmt]
                    # Try the next format rather than dropping the post - a 2 MB .slc still loses
                    # to a .gdr that fits.
                    if size < 0:
                        rejected.append(fmt + ":unreadable-size"); continue
                    if size > SIZE_CAP[fmt]:
                        rejected.append("%s:%dKB" % (fmt, size // 1024)); continue
                    pick = (fmt, fn, size, did)
                    break
            if not pick:
                have = ",".join(sorted(files)) or "none"
                # Distinguish "we cannot read anything here" from "we could, but it was too big".
                # Reporting both as no-supported-format hid real coverage behind a wrong label.
                if rejected:
                    skip(c, mid, "oversize[%s]" % ",".join(rejected))
                else:
                    skip(c, mid, "no-supported-format[%s]" % have)
                skipped += 1
                continue

            fmt, fn, size, doc_id = pick

            # The caption-to-document pairing is an undocumented property of how Telegram splits an
            # album into bubbles. It is right ~99% of the time, but if it ever shifts by one, every
            # row silently takes the previous post's level id - and for .slc there is no embedded
            # id to catch it later. So check it: the filename is '<levelname>.<ext>', and caption
            # name vs file basename agree on 99.2% of real posts. Cheap, and it converts a silent
            # structural assumption into a checked one.
            # Compare NORMALISED, not raw. The channel's filenames drop spaces, swap in
            # underscores, strip punctuation and change case relative to the caption - so a raw
            # comparison rejects "Cat Planet"/"cat_planet" and "A.G.I.T.E"/"Agite" as if they were
            # different levels. Containment rather than equality also keeps the common
            # "<name>" / "Unnerfed <name>" variant pairs.
            norm = lambda s: re.sub(r"[^a-z0-9]", "", (s or "").lower())
            base = norm(re.sub(r"\s*\(\d+\)$", "", fn.rsplit(".", 1)[0]))
            nm = norm(info["name"])
            paired = (not nm or not base
                      or base.startswith(nm[:8]) or nm.startswith(base[:8])
                      or nm in base or base in nm)
            if not paired:
                skip(c, mid, "pair-unverified"); skipped += 1; continue

            c.execute(
                "INSERT INTO posts (msg_id, doc_msg_id, caption_id, level_id, name, credit, "
                "showcase, difficulty, format, filename, bytes, posted) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                (mid, doc_id, cap_id, info["level_id"], info["name"], info["credit"],
                 info["showcase"], info["difficulty"], fmt, fn, size, posted))
            # A post that now succeeds must lose its old skip row, or the skip histogram keeps
            # reporting failures that have since been fixed - and that histogram is the only
            # signal that the channel has changed shape.
            c.execute("DELETE FROM skipped WHERE msg_id = ?", (mid,))
            added += 1
            page_new += 1

        # Only advance the resume cursor past posts that are fully dealt with. Anything still in
        # `pending` is waiting for a caption on the next page, so the cursor must not skip it.
        settled = min([lowest] + [m - 1 for m, _, _ in pending])
        c.execute("INSERT OR REPLACE INTO state (key,value) VALUES ('oldest_seen',?)", (str(settled),))
        c.commit()

        pages += 1
        print("  page %-4d before=%-7s posts+%-4d skip+%-4d  (oldest id %d)"
              % (pages, before or "top", added, skipped, lowest))

        if page_new == 0:
            dry_pages += 1
            # An incremental run is caught up once two whole pages hold nothing new. A --full run
            # keeps going: gaps in the middle of the archive are expected and are not the end.
            if not args.full and dry_pages >= 2:
                print("  no new posts for 2 pages - caught up")
                break
        else:
            dry_pages = 0

        if before is not None and lowest >= before:
            stall += 1
            if stall >= 2:
                print("  pagination stopped advancing - done")
                break
        else:
            stall = 0
        before = lowest
        if lowest <= 1:
            break
        if args.pages and pages >= args.pages:
            break
        time.sleep(1.0)      # ordinary politeness; this endpoint is not rate limited in practice

    # Whatever is still pending never found its caption - the run ended before the page that would
    # have carried it. Record it rather than dropping it silently, which would lose the oldest post
    # of every single run.
    for mid, _d, _p in pending:
        skip(c, mid, "no-caption-run-ended")
        skipped += 1
    c.commit()

    # This run, separately from the database totals. A scrape that dies on page 1 otherwise prints
    # the whole backfill's numbers and looks like a success.
    print("\nthis run: %d pages, %d new posts, %d skipped" % (pages, added, skipped))
    if pages == 0:
        print("FAILED: no pages fetched")
        return 1
    if added == 0 and skipped == 0:
        print("nothing new (this is normal for an incremental run with no new posts)")

    tot = c.execute("SELECT COUNT(*) FROM posts").fetchone()[0]
    byf = c.execute("SELECT format, COUNT(*) FROM posts GROUP BY format").fetchall()
    lv = c.execute("SELECT COUNT(DISTINCT level_id) FROM posts").fetchone()[0]
    mb = (c.execute("SELECT COALESCE(SUM(bytes),0) FROM posts").fetchone()[0]) / 1048576.0
    print("\nindexed %d posts over %d levels, %.1f MB to mirror" % (tot, lv, mb))
    for f, n in byf:
        print("   %-6s %d" % (f, n))
    print("\nskip reasons:")
    for r, n in c.execute("SELECT reason, COUNT(*) FROM skipped GROUP BY reason ORDER BY 2 DESC"):
        print("   %-28s %d" % (r, n))
    c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
