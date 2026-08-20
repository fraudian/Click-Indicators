"""Narrow the Silicate v1 layout using levels whose real length is known.

Monotonicity and alternation cannot separate the candidates - any right shift preserves both. What
does separate them is absolute time: Slaughterhouse is about 2 minutes, so whichever shift puts its
last input near 120 s is the right one, and the others are wrong by a clean factor of two.
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "slcv1_samples")

# rough real durations, for orientation only
KNOWN = {
    "Slaughterhouse": 122, "The Big Black": 90, "Jawbreaker": 107,
    "Static Ignition": 100, "Destinies": 133, "Rainstorm": 90,
}

import sqlite3
c = sqlite3.connect(os.path.join(HERE, "work.db"))
names = dict(c.execute("SELECT msg_id, filename FROM posts"))

print("%-26s %-7s %-6s %-7s %s" % ("file", "tps", "count", "clicks", "duration under frame=v>>N"))
print("%-26s %-7s %-6s %-7s %-9s %-9s %-9s %-9s"
      % ("", "", "", "", "N=0", "N=1", "N=2", "N=3"))
for fn in sorted(os.listdir(CACHE)):
    if not fn.endswith(".slc"):
        continue
    msg = int(fn[:-4])
    data = open(os.path.join(CACHE, fn), "rb").read()
    tps = struct.unpack("<d", data[0:8])[0]
    count = struct.unpack("<I", data[8:12])[0]
    body = data[12:]
    if len(body) != count * 4:
        print("  %s: body %d != count*4 %d" % (fn, len(body), count * 4))
        continue
    last = struct.unpack("<I", body[-4:])[0]
    first = struct.unpack("<I", body[0:4])[0]
    label = (names.get(msg, fn) or fn)[:26]
    durs = ["%.0fs" % ((last >> n) / tps) for n in (0, 1, 2, 3)]
    print("%-26s %-7.0f %-6d %-7d %-9s %-9s %-9s %-9s"
          % (label, tps, count, count // 2, *durs))
    print("%-26s %-7s %-6s %-7s first-record frame under each: %s"
          % ("", "", "", "", ", ".join("%.2fs" % ((first >> n) / tps) for n in (0, 1, 2, 3))))

print("\nreference real lengths:", ", ".join("%s~%ds" % (k, v) for k, v in KNOWN.items()))
