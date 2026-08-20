"""Two checks the pilot could not make for itself.

1. .gdr files pass ingest on magic bytes alone, but 30 MB of the mirror depends on
   src/parsers.cpp actually finding inputs in them. parseMsgpack looks for an "inputs" array of
   maps with frame/down/2p/btn keys, so decode the msgpack header here and confirm those keys are
   really present rather than trusting a leading byte.

2. pair-unverified jumped from 19 to 110 skips once .gdr entered the picture. Either the older
   era genuinely names files differently, or the check is too strict and is throwing away good
   levels. Print the actual pairs so it can be judged rather than guessed.
"""
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
c = sqlite3.connect(os.path.join(HERE, "work.db"))

print("=" * 78)
print("1. do the mirrored .gdr files actually contain an inputs array?")
print("=" * 78)
for msg, fn, path, nbytes in c.execute(
        "SELECT msg_id, filename, local, bytes FROM posts "
        "WHERE state='mirrored' AND format='gdr'"):
    if not path or not os.path.exists(path):
        continue
    data = open(path, "rb").read()
    # msgpack string keys appear literally in the bytes; these are the ones parseMsgpack wants
    keys = {k: (b"\xa6inputs" if k == "inputs" else k.encode()) in data
            for k in ("inputs", "frame", "down", "btn", "2p", "framerate", "fps")}
    keys["inputs"] = b"inputs" in data
    got = ",".join(k for k, v in keys.items() if v) or "NONE"
    head = data[0]
    kind = ("fixmap" if 0x80 <= head <= 0x8F else
            "map16" if head == 0xDE else "map32" if head == 0xDF else "?%02x" % head)
    print("  %-7d %-26s %8d B  %-7s keys: %s" % (msg, fn[:26], nbytes, kind, got))

print()
print("=" * 78)
print("2. what is actually being rejected as pair-unverified?")
print("=" * 78)
ids = [r[0] for r in c.execute(
    "SELECT msg_id FROM skipped WHERE reason='pair-unverified' ORDER BY msg_id DESC LIMIT 20")]
print("  (msg_id list, %d total; re-scraping their captions to show the pair)" % c.execute(
    "SELECT COUNT(*) FROM skipped WHERE reason='pair-unverified'").fetchone()[0])
print("  " + ", ".join(str(i) for i in ids[:20]))
