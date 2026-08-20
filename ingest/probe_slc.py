"""Fetch a handful of the .slc files that were rejected as bad-magic and dump their real headers.

The C++ reader (src/parsers.cpp) accepts "SLC3RPLY" (v3) and "SILL" (v2). The mirror's validator
copies that test, and the 2026-era files pass it - so whatever these older files start with, both
readers are currently wrong about them, and that is worth knowing before 1,200 levels are dropped.
"""
import os
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mirror_telegram import get_client, CHANNEL  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
c = sqlite3.connect(os.path.join(HERE, "work.db"))

rows = c.execute(
    "SELECT p.doc_msg_id, p.msg_id, p.filename, p.bytes FROM posts p "
    "JOIN skipped s ON s.msg_id = p.msg_id "
    "WHERE s.reason = 'bad-magic' AND p.format = 'slc' "
    "ORDER BY p.msg_id DESC"
).fetchall()
print("bad-magic .slc rows: %d" % len(rows))

# spread the sample across the whole affected range, not just the newest
step = max(1, len(rows) // 8)
sample = rows[::step][:8]

client = get_client()
client.start()
ent = client.get_entity(CHANNEL)
msgs = client.get_messages(ent, ids=[r[0] for r in sample])

print("\n%-8s %-28s %-8s %s" % ("msg", "filename", "bytes", "first 24 bytes"))
for row, msg in zip(sample, msgs):
    if msg is None:
        continue
    data = client.download_media(msg, file=bytes)
    if not data:
        continue
    printable = "".join(chr(b) if 32 <= b < 127 else "." for b in data[:16])
    print("%-8d %-28s %-8d %s" % (row[1], row[2][:28], len(data), data[:24].hex(" ")))
    print("%-8s %-28s %-8s ascii: %r" % ("", "", "", printable))

client.disconnect()
