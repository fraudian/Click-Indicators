"""Work out the Silicate v1 record layout from real files, rather than guessing at it.

v1 has no magic: f64 tps, u32 count, then count * 4-byte records. The question is what those 4
bytes mean. Rather than assume, decode every candidate layout and score it against properties a
real macro must have: frames that never go backwards, presses and releases that alternate, and a
total duration that is plausible for a Geometry Dash level.
"""
import os
import struct
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "slcv1_samples")
sys.path.insert(0, HERE)


def fetch_samples(n=10):
    os.makedirs(CACHE, exist_ok=True)
    have = [f for f in os.listdir(CACHE) if f.endswith(".slc")]
    if len(have) >= n:
        return [os.path.join(CACHE, f) for f in have]
    from mirror_telegram import get_client, CHANNEL
    c = sqlite3.connect(os.path.join(HERE, "work.db"))
    rows = c.execute(
        "SELECT p.doc_msg_id, p.msg_id FROM posts p JOIN skipped s ON s.msg_id = p.msg_id "
        "WHERE s.reason='bad-magic' AND p.format='slc' ORDER BY p.msg_id DESC"
    ).fetchall()
    step = max(1, len(rows) // n)
    sample = rows[::step][:n]
    client = get_client(); client.start()
    ent = client.get_entity(CHANNEL)
    for row, msg in zip(sample, client.get_messages(ent, ids=[r[0] for r in sample])):
        if msg is None:
            continue
        data = client.download_media(msg, file=bytes)
        if data:
            open(os.path.join(CACHE, "%d.slc" % row[1]), "wb").write(data)
    client.disconnect()
    return [os.path.join(CACHE, f) for f in os.listdir(CACHE) if f.endswith(".slc")]


LAYOUTS = {
    "frame=v>>1, down=v&1":            lambda v: (v >> 1, v & 1, 0),
    "frame=v>>2, down=v&1, p2=(v>>1)": lambda v: (v >> 2, v & 1, (v >> 1) & 1),
    "frame=v>>2, down=(v>>1), p2=v&1": lambda v: (v >> 2, (v >> 1) & 1, v & 1),
    "frame=v>>3, down=v&1":            lambda v: (v >> 3, v & 1, 0),
    "frame=v (no flags)":              lambda v: (v, -1, 0),
}


def score(path, fn):
    data = open(path, "rb").read()
    tps = struct.unpack("<d", data[0:8])[0]
    count = struct.unpack("<I", data[8:12])[0]
    body = data[12:]
    if len(body) != count * 4:
        return None
    frames, downs = [], []
    for i in range(count):
        v = struct.unpack("<I", body[i * 4:i * 4 + 4])[0]
        f, d, _p2 = fn(v)
        frames.append(f); downs.append(d)
    monotonic = all(frames[i] <= frames[i + 1] for i in range(len(frames) - 1))
    alt = 0
    if downs[0] != -1:
        alt = sum(1 for i in range(len(downs) - 1) if downs[i] != downs[i + 1]) / max(1, len(downs) - 1)
    dur = frames[-1] / tps if tps else 0
    return {"tps": tps, "count": count, "monotonic": monotonic,
            "alternation": alt, "duration_s": dur,
            "plausible": monotonic and 5 < dur < 900}


files = fetch_samples()
print("samples: %d\n" % len(files))
for name, fn in LAYOUTS.items():
    ok = 0
    detail = []
    for p in files:
        r = score(p, fn)
        if r is None:
            continue
        if r["plausible"]:
            ok += 1
        detail.append(r)
    if not detail:
        continue
    avg_alt = sum(d["alternation"] for d in detail) / len(detail)
    print("%-34s plausible %2d/%-3d  mono %2d  avg-alternation %.2f  durations %s"
          % (name, ok, len(detail),
             sum(1 for d in detail if d["monotonic"]),
             avg_alt,
             ", ".join("%.0fs" % d["duration_s"] for d in detail[:5])))
