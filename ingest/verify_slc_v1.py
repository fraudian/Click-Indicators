"""Decisive test of the Silicate v1 shift: compare against .gdr2 files for the SAME level.

The .gdr2 decoder is already proven correct (it recovers the real GD level ids for 47 files and its
decoded input counts match each file's own declared count). So for a level that appears as both a
v1 .slc and a .gdr2, the run lengths must agree - two people playing the same level produce
different click patterns but the same level duration. Only the correct shift will match.
"""
import os
import struct
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mirror_telegram import get_client, CHANNEL, decode_gdr2  # noqa: E402

c = sqlite3.connect(os.path.join(HERE, "work.db"))
pairs = c.execute(
    "SELECT a.level_id, a.doc_msg_id, a.filename, b.local, b.filename "
    "FROM posts a "
    "JOIN skipped s ON s.msg_id = a.msg_id AND s.reason = 'bad-magic' "
    "JOIN posts b ON b.level_id = a.level_id AND b.format = 'gdr2' AND b.state != 'indexed' "
    "WHERE a.format = 'slc' AND b.local IS NOT NULL"
).fetchall()
pairs = [p for p in pairs if p[3] and os.path.exists(p[3])][:8]
print("comparable pairs: %d\n" % len(pairs))
if not pairs:
    sys.exit("no gdr2 blobs mirrored yet for these levels")

client = get_client()
client.start()
ent = client.get_entity(CHANNEL)
msgs = client.get_messages(ent, ids=[p[1] for p in pairs])

print("%-24s %-11s %-9s | slc duration at shift" % ("level", "gdr2 dur", "gdr2 clk"))
print("%-24s %-11s %-9s | %-9s %-9s %-9s %-9s" % ("", "", "", "2", "3", "4", "5"))
best = {2: 0, 3: 0, 4: 0, 5: 0}
for (lvl, doc, slcname, g2path, g2name), msg in zip(pairs, msgs):
    if msg is None:
        continue
    data = client.download_media(msg, file=bytes)
    if not data or len(data) < 12:
        continue
    tps = struct.unpack("<d", data[0:8])[0]
    count = struct.unpack("<I", data[8:12])[0]
    body = data[12:]
    if len(body) != count * 4:
        continue
    last = struct.unpack("<I", body[-4:])[0]

    g2 = open(g2path, "rb").read()
    _lid, gfps, ginputs = decode_gdr2(g2)
    # last frame of the gdr2, decoded the same way importData does
    r = M_last_frame = None
    from mirror_telegram import Reader
    rr = Reader(g2); rr.i = 3
    rr.varint(); tag = rr.string(); rr.string(); rr.string(); rr.f32(); rr.varint()
    rr.f64(); rr.varint(); rr.varint(); rr.varint(1); rr.varint(1)
    rr.string(); rr.varint(); rr.varint(); rr.string()
    ext = rr.varint(); rr.i += ext
    deaths = rr.varint()
    for _ in range(min(deaths, len(g2))):
        rr.varint(8)
    rr.varint(); rr.varint()
    frame = 0
    while rr.i < len(g2):
        before = rr.i
        try:
            packed = rr.varint(8)
        except ValueError:
            break
        if rr.i == before:
            break
        frame += packed >> 1
        if tag:
            e = rr.varint()
            if e > len(g2) - rr.i:
                break
            rr.i += e
    gdur = frame / gfps

    durs = [(last >> n) / tps for n in (2, 3, 4, 5)]
    # which shift lands closest to the gdr2's duration?
    win = [2,3,4,5][min(range(4), key=lambda i: abs(durs[i] - gdur))]
    best[win] += 1
    print("%-24s %-11.1f %-9d | %-9s %-9s %-9s %-9s  -> shift %d"
          % ((slcname or "")[:24], gdur, ginputs,
             *["%.0fs" % d for d in durs], win))

client.disconnect()
print("\nclosest-match tally:", ", ".join("shift %d: %d" % (k, v) for k, v in best.items()))
