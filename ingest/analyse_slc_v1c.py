"""Decide the Silicate v1 shift using hold durations, which are self-validating.

Records alternate press/release, so the gap between record 2k and 2k+1 is how long a finger was
down. Humans hold a jump for roughly 30-250 ms and essentially never for multiple seconds, so only
one shift can produce a believable distribution. This needs no external knowledge of level lengths.
"""
import os
import statistics
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "slcv1_samples")

for shift in (0, 1, 2, 3):
    holds, gaps = [], []
    for fn in sorted(os.listdir(CACHE)):
        if not fn.endswith(".slc"):
            continue
        d = open(os.path.join(CACHE, fn), "rb").read()
        tps = struct.unpack("<d", d[0:8])[0]
        count = struct.unpack("<I", d[8:12])[0]
        body = d[12:]
        if len(body) != count * 4:
            continue
        vals = [struct.unpack("<I", body[i * 4:i * 4 + 4])[0] for i in range(count)]
        fr = [v >> shift for v in vals]
        for i in range(0, len(fr) - 1, 2):
            holds.append((fr[i + 1] - fr[i]) / tps)
        for i in range(1, len(fr) - 1, 2):
            gaps.append((fr[i + 1] - fr[i]) / tps)
    if not holds:
        continue
    holds.sort()
    plausible = sum(1 for h in holds if 0.015 <= h <= 0.40) / len(holds)
    print("shift %d : hold median %6.3fs  p10 %6.3fs  p90 %6.3fs   %.0f%% of holds in 15-400ms"
          % (shift, statistics.median(holds), holds[len(holds) // 10],
             holds[len(holds) * 9 // 10], 100 * plausible))
    print("          gap between clicks: median %6.3fs" % statistics.median(gaps))
