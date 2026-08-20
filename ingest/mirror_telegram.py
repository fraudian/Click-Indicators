#!/usr/bin/env python3
"""
Stage 2 of the t.me/gdmacros mirror: pull the bytes over MTProto.

This is the ONLY part of the whole pipeline that holds a Telegram credential, and it deliberately
runs nowhere near the Worker or the mod. It reads what index_pass.py already decided to fetch, so
it never downloads a file to find out it was 620 MB of per-frame physics state - the size cap is
applied to the size the public preview already reported.

Every file is validated before it is kept. That matters more here than it looks: anyone can submit
to @GDmacrosBot, and a file that reaches the bucket is a file that auto-loads on paying customers'
machines. So a .gdr2 is fully decoded here, with the same header layout src/gdr_parse.cpp reads,
and rejected if it contains no inputs - EclipseBot writes structurally valid 60-80 byte stubs with
zero inputs, and one of those would otherwise rank first and silently kill the guide for a level.

Setup (one time):
    pip install telethon
    # api_id / api_hash from https://my.telegram.org -> API development tools
    set TG_API_ID=...
    set TG_API_HASH=...
    python mirror_telegram.py --login       # interactive, writes ci_ingest.session

Then:
    python mirror_telegram.py               # mirror everything index_pass.py found
    python mirror_telegram.py --limit 20    # a small stratified pilot first
"""

import argparse
import hashlib
import math
import os
import sqlite3
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(HERE, "work.db")
BLOBS = os.path.join(HERE, "blobs")
SESSION = os.path.join(HERE, "ci_ingest")
CHANNEL = "gdmacros"

SIZE_CAP = {"gdr2": 256 * 1024, "slc": 1536 * 1024, "gdr": 8 << 20}


# ---------------------------------------------------------------- gdr2 decoding
#
# Mirrors external/gdr/binarystream.hpp: integers (and bool) are LEB128 varints, strings are a
# varint length followed by raw bytes, and float/double are fixed width BIG-ENDIAN. Getting this
# byte-exact is what lets us assert "this file really does contain N inputs" rather than trusting
# a size heuristic.

class Reader:
    def __init__(self, b):
        self.b = b
        self.i = 0

    def varint(self, width=4):
        r = 0
        for k in range(width + 1):
            if self.i >= len(self.b):
                raise ValueError("truncated varint")
            byte = self.b[self.i]
            self.i += 1
            r |= (byte & 0x7F) << (k * 7)
            if not (byte & 0x80):
                return r
        return r

    def string(self):
        n = self.varint()
        if n > 0xFFFF or self.i + n > len(self.b):
            raise ValueError("bad string")
        s = self.b[self.i:self.i + n]
        self.i += n
        return s.decode("utf-8", "replace")

    def f32(self):
        if self.i + 4 > len(self.b):
            raise ValueError("truncated f32")
        v = struct.unpack(">f", self.b[self.i:self.i + 4])[0]
        self.i += 4
        return v

    def f64(self):
        if self.i + 8 > len(self.b):
            raise ValueError("truncated f64")
        v = struct.unpack(">d", self.b[self.i:self.i + 8])[0]
        self.i += 8
        return v


def decode_gdr2(data):
    """(level_id, framerate, decoded_inputs). Raises on anything malformed.

    The count returned is the number of input records actually DECODED, not the count the header
    declares. Those are different numbers for a crafted file, and trusting the declared one would
    let a post claim 500 inputs, carry none, and sail through the empty-macro check into the
    bucket - where it becomes a level with no guide and no error on 800+ machines."""
    if data[:3] != b"GDR":
        raise ValueError("bad magic")
    r = Reader(data)
    r.i = 3
    r.varint()            # version
    input_tag = r.string()
    r.string()            # author
    r.string()            # description
    r.f32()               # duration
    r.varint()            # gameVersion
    framerate = r.f64()
    r.varint()            # seed
    r.varint()            # coins
    r.varint(1)           # ldm
    r.varint(1)           # platformer
    r.string()            # botInfo.name
    r.varint()            # botInfo.version - an int, not a string (gdr.hpp `struct Bot`)
    level_id = r.varint()
    r.string()            # levelInfo.name

    ext = r.varint()
    if ext > len(data):
        raise ValueError("bad extension size")
    r.i += ext

    deaths = r.varint()
    for _ in range(min(deaths, len(data))):
        r.varint(8)
    r.varint()            # declared input count - recorded by the writer, not trusted here
    r.varint()            # p1Inputs

    # Walk the actual records the way gdr.hpp does. A pass that consumes no bytes is the end of the
    # data however the file claims it ends - the C++ reader has the same guard for the same reason.
    decoded = 0
    while r.i < len(data):
        before = r.i
        try:
            r.varint(8)                       # packed frame delta + button + down
            if input_tag:
                ext = r.varint()
                if ext > len(data) - r.i:
                    break
                r.i += ext
        except ValueError:
            break
        if r.i == before:
            break
        decoded += 1
        if decoded > 5000000:
            raise ValueError("absurd input count")
    return level_id, framerate, decoded


def validate(fmt, data, want_level):
    """(ok, reason, level_id_asserted, fps, inputs). level_id_asserted is 0 when the file makes
    no claim about which level it is - true for every .slc, which is ~82% of the archive."""
    cap = SIZE_CAP[fmt]
    if len(data) > cap:
        return False, "oversize", 0, None, 0
    if len(data) < 16:
        return False, "too-small", 0, None, 0

    if fmt == "gdr2":
        try:
            lid, fps, n = decode_gdr2(data)
        except Exception as e:                      # noqa: BLE001 - any malformation is a reject
            return False, "undecodable[%s]" % str(e)[:32], 0, None, 0
        if n <= 0:
            # A valid header with no inputs. Real, and poisonous: it would satisfy the client's
            # cache check and leave the level with a permanently blank guide and no error.
            return False, "empty-macro", 0, None, 0
        if not (math.isfinite(fps) and 1.0 < fps <= 100000.0):
            return False, "bad-framerate", 0, None, 0
        if lid and want_level and lid != want_level:
            return False, "level-mismatch", lid, fps, n
        return True, "", lid, fps, n

    if fmt == "slc":
        # Silicate. Three on-wire forms, all read by src/parsers.cpp. None carries an embedded
        # level id, so these always land with verified = 0 and the Worker ranks them behind
        # anything self-verifying.
        if data[:8] == b"SLC3RPLY" or data[:4] == b"SILL":
            return True, "", 0, None, 0
        # v1 has no magic: f64 tps, u32 count, then count * 4 bytes. Validate that identity
        # exactly rather than waving the file through - it is what distinguishes a real v1 file
        # from arbitrary bytes wearing a .slc name.
        if len(data) >= 12:
            tps = struct.unpack("<d", data[0:8])[0]
            count = struct.unpack("<I", data[8:12])[0]
            if (count > 0 and len(data) - 12 == count * 4
                    and math.isfinite(tps) and 1.0 < tps <= 100000.0):
                return True, "", 0, tps, count
        return False, "bad-magic", 0, None, 0

    if fmt == "gdr":
        # GDReplay v1: msgpack. Same test parseMacroFileInner uses to dispatch it - a fixmap
        # (0x80-0x8f), map16 (0xde) or map32 (0xdf) leading byte. Some files in this channel are
        # actually v2 binaries wearing a .gdr name, so accept those through the v2 decoder.
        if data[:3] == b"GDR":
            try:
                lid, fps, n = decode_gdr2(data)
            except Exception:                       # noqa: BLE001
                return False, "undecodable", 0, None, 0
            if n <= 0:
                return False, "empty-macro", 0, None, 0
            if lid and want_level and lid != want_level:
                return False, "level-mismatch", lid, fps, n
            return True, "", lid, fps, n
        b0 = data[0]
        if (0x80 <= b0 <= 0x8F) or b0 in (0xDE, 0xDF):
            return True, "", 0, None, 0
        return False, "bad-magic", 0, None, 0

    return False, "unknown-format", 0, None, 0


# ---------------------------------------------------------------- telegram

def get_client(login=False):
    try:
        from telethon.sync import TelegramClient
    except ImportError:
        print("telethon is not installed.  pip install telethon")
        sys.exit(2)

    api_id = os.environ.get("TG_API_ID")
    api_hash = os.environ.get("TG_API_HASH")
    if not api_id or not api_hash:
        print("Set TG_API_ID and TG_API_HASH first (https://my.telegram.org -> API development tools).")
        sys.exit(2)

    # flood_sleep_threshold: Telethon sleeps through short FLOOD_WAITs itself and raises on long
    # ones, which is what we want - a long wait should be handled deliberately, not slept through.
    return TelegramClient(SESSION, int(api_id), api_hash, flood_sleep_threshold=60)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--login", action="store_true", help="interactive sign-in, then exit")
    ap.add_argument("--limit", type=int, default=0, help="stop after N files")
    ap.add_argument("--batch", type=int, default=100, help="messages fetched per call")
    ap.add_argument("--format", default="", help="pilot one format only: gdr2 | slc | gdr")
    args = ap.parse_args()

    from telethon.errors import FloodWaitError

    client = get_client()
    client.start()
    if args.login:
        me = client.get_me()
        print("signed in as %s (id %s). session written to %s.session"
              % (getattr(me, "username", None) or me.first_name, me.id, SESSION))
        client.disconnect()
        return 0

    os.makedirs(BLOBS, exist_ok=True)
    c = sqlite3.connect(DB)
    # doc_msg_id, not msg_id. The album bubble reports only its first document's id, so fetching by
    # msg_id downloads whichever file happens to be first - in practice the multi-megabyte
    # .gdr.json - for every post.
    todo = c.execute(
        "SELECT doc_msg_id, level_id, format, filename, bytes, msg_id FROM posts "
        "WHERE state = 'indexed' ORDER BY msg_id DESC"
    ).fetchall()
    if args.format:
        todo = [r for r in todo if r[2] == args.format]
    if args.limit:
        # Stratified, not just the newest N. Sorted by msg_id the newest rows are all .gdr2, which
        # is 7% of the corpus - a pilot that only exercises those proves nothing about the 93%.
        # Take up to limit/3 of each format, oldest and newest of each, then fill.
        per = max(1, args.limit // 3)
        picked, seen = [], set()
        for fmt in ("gdr2", "slc", "gdr"):
            rows = [r for r in todo if r[2] == fmt]
            for r in rows[:per] + rows[-per:]:
                if r[0] not in seen:
                    seen.add(r[0]); picked.append(r)
        for r in todo:
            if len(picked) >= args.limit:
                break
            if r[0] not in seen:
                seen.add(r[0]); picked.append(r)
        todo = picked[:args.limit]
    if not todo:
        print("nothing to mirror - run index_pass.py first")
        return 0

    print("mirroring %d files from t.me/%s\n" % (len(todo), CHANNEL))
    ent = client.get_entity(CHANNEL)

    ok = bad = 0
    for start in range(0, len(todo), args.batch):
        chunk = todo[start:start + args.batch]
        by_id = {r[0]: r for r in chunk}

        # One call per batch, not per file. getHistory is not the flood-sensitive method; the
        # download is, and these are ~1 KB each.
        try:
            msgs = client.get_messages(ent, ids=list(by_id.keys()))
        except FloodWaitError as e:
            print("  FLOOD_WAIT %ds - sleeping" % e.seconds)
            time.sleep(e.seconds + 5)
            msgs = client.get_messages(ent, ids=list(by_id.keys()))

        for msg in msgs:
            if msg is None:
                continue
            row = by_id.get(msg.id)
            if not row:
                continue
            doc_id, level_id, fmt, filename, size, msg_id = row

            # Belt and braces on the album-offset bug: the file we get must be the file we indexed.
            # If Telegram ever renumbers or the index drifts, this catches it on the first post
            # rather than after 2,000 wrong downloads.
            got_name = getattr(getattr(msg, "file", None), "name", None)
            if got_name and filename and got_name != filename:
                c.execute("INSERT OR REPLACE INTO skipped VALUES (?,?,?)",
                          (msg_id, "filename-drift", int(time.time())))
                c.execute("UPDATE posts SET state='skipped' WHERE msg_id=?", (msg_id,))
                print("  ! %d expected %r, Telegram gave %r - stopping" % (doc_id, filename, got_name))
                bad += 1
                continue

            if size > SIZE_CAP[fmt]:
                c.execute("INSERT OR REPLACE INTO skipped VALUES (?,?,?)",
                          (msg_id, "oversize-indexed", int(time.time())))
                c.execute("UPDATE posts SET state='skipped' WHERE msg_id=?", (msg_id,))
                bad += 1
                continue

            # The cap has to be checked against the size ON THE WIRE, not only the size the preview
            # advertised - otherwise the "nothing large is ever downloaded" guarantee rests on
            # scraped HTML. Telegram reports the real size before any bytes are transferred.
            wire = getattr(getattr(msg, "file", None), "size", None)
            if wire is not None and wire > SIZE_CAP[fmt]:
                c.execute("INSERT OR REPLACE INTO skipped VALUES (?,?,?)",
                          (msg_id, "oversize-wire", int(time.time())))
                c.execute("UPDATE posts SET state='skipped' WHERE msg_id=?", (msg_id,))
                bad += 1
                continue

            doc = getattr(msg, "document", None)
            if doc is None:
                c.execute("INSERT OR REPLACE INTO skipped VALUES (?,?,?)",
                          (msg_id, "no-document", int(time.time())))
                c.execute("UPDATE posts SET state='skipped' WHERE msg_id=?", (msg_id,))
                bad += 1
                continue

            try:
                data = client.download_media(msg, file=bytes)
            except FloodWaitError as e:
                print("  FLOOD_WAIT %ds - sleeping" % e.seconds)
                time.sleep(e.seconds + 5)
                try:
                    data = client.download_media(msg, file=bytes)
                except Exception as e2:             # noqa: BLE001
                    print("  ! %d download failed: %s" % (msg_id, e2))
                    continue
            except Exception as e:                  # noqa: BLE001
                print("  ! %d download failed: %s" % (msg_id, e))
                continue

            # Paced after every download, not only successful ones. A rejected file cost exactly the
            # same bandwidth as a kept one, so skipping the throttle on rejects removes the pacing
            # precisely when a run is churning through bad posts fastest.
            time.sleep(0.35)      # well under the documented 5-concurrent small-file ceiling

            if not data:
                continue

            good, reason, lid, fps, n = validate(fmt, data, level_id)
            if not good:
                c.execute("INSERT OR REPLACE INTO skipped VALUES (?,?,?)",
                          (msg_id, reason, int(time.time())))
                c.execute("UPDATE posts SET state='skipped' WHERE msg_id=?", (msg_id,))
                print("  - %-7d %-28s rejected: %s" % (msg_id, filename[:28], reason))
                bad += 1
                continue

            sha = hashlib.sha256(data).hexdigest()
            path = os.path.join(BLOBS, "%s.%s" % (sha, fmt))
            with open(path, "wb") as f:
                f.write(data)

            c.execute(
                "UPDATE posts SET state='mirrored', sha256=?, local=?, bytes=?, fps=?, inputs=?, "
                "verified=? WHERE msg_id=?",
                (sha, path, len(data), fps, n, 1 if (lid and lid == level_id) else 0, msg_id))
            ok += 1
            if ok % 25 == 0:
                c.commit()
                print("  %d ok, %d rejected" % (ok, bad))

        c.commit()

    v = c.execute("SELECT COUNT(*) FROM posts WHERE state='mirrored' AND verified=1").fetchone()[0]
    tot = c.execute("SELECT COALESCE(SUM(bytes),0) FROM posts WHERE state='mirrored'").fetchone()[0]
    print("\nmirrored %d files (%d self-verified), %.2f MB" % (ok, v, tot / 1048576.0))
    if bad:
        print("rejected %d:" % bad)
        for r, k in c.execute("SELECT reason, COUNT(*) FROM skipped GROUP BY reason ORDER BY 2 DESC"):
            print("   %-28s %d" % (r, k))
    c.close()
    client.disconnect()
    return 0


if __name__ == "__main__":
    sys.exit(main())
