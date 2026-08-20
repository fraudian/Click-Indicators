"""Does a real wave ever leave a slope of exactly +-1 / +-2?

That is the whole D-block question. If waves slide along block surfaces, the recorded per-frame
positions must show stretches where dy/dx is neither +-1 nor +-2 - it would be the surface's
gradient instead. If |dy/dx| is always 1 or 2 across every recorded wave frame, then whatever
D-blocks do, they do not bend the trajectory, and the route solver needs no new physics.

Measured over the mirrored .gdr files that carry frameFixes (the real x/y of the real run).
"""
import glob
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_traj import load  # noqa: E402

files = sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'blobs', '*.gdr')))

tot = 0
onGrid = 0
buckets = Counter()
odd_runs = []          # (file, startFrame, length, slope) for sustained off-grid stretches
scanned = 0

for f in files:
    o = load(f)
    if not isinstance(o, dict):
        continue
    fixes = o.get('frameFixes')
    if not fixes:
        continue
    pos = {}
    for fx in fixes:
        if isinstance(fx, dict) and 'frame' in fx:
            p1 = fx.get('p1') or {}
            if 'x' in p1 and 'y' in p1:
                pos[fx['frame']] = (p1['x'], p1['y'])
    if len(pos) < 500:
        continue
    scanned += 1
    frames = sorted(pos)

    run_len = 0
    run_slope = None
    run_start = None
    for k in range(1, len(frames)):
        a, b = frames[k - 1], frames[k]
        if b != a + 1:
            run_len = 0; run_slope = None; continue
        x0, y0 = pos[a]; x1, y1 = pos[b]
        dx = x1 - x0
        if dx <= 0.0001:
            run_len = 0; run_slope = None; continue
        s = abs((y1 - y0) / dx)
        # a wave frame is one where the magnitude is a wave magnitude, OR something else entirely
        if s < 0.02:
            # flat - cube on ground, ship cruising, etc. Not a wave frame; ignore.
            run_len = 0; run_slope = None; continue
        tot += 1
        if abs(s - 1.0) < 0.02 or abs(s - 2.0) < 0.02:
            onGrid += 1
            run_len = 0; run_slope = None
        else:
            buckets[round(s, 1)] += 1
            if run_slope is not None and abs(s - run_slope) < 0.05:
                run_len += 1
            else:
                run_slope = s; run_len = 1; run_start = a
            if run_len == 6:      # six consecutive frames at one non-wave gradient
                odd_runs.append((os.path.basename(f)[:22], run_start, run_slope))

print("files with recorded positions : %d" % scanned)
print("non-flat frames measured      : %d" % tot)
if tot:
    print("on the wave grid (|s|=1 or 2)  : %d  (%.3f%%)" % (onGrid, 100.0 * onGrid / tot))
    print("off grid                       : %d  (%.3f%%)" % (tot - onGrid, 100.0 * (tot - onGrid) / tot))
print()
print("most common OFF-GRID gradients (these would be slides, or other gamemodes):")
for s, n in buckets.most_common(12):
    print("   |dy/dx| = %-5s  %d frames" % (s, n))
print()
print("sustained off-grid stretches (>=6 frames at one gradient): %d" % len(odd_runs))
for r in odd_runs[:10]:
    print("   %-24s frame %-7d gradient %.3f" % r)
