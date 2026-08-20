"""Narrow down WHAT breaks the walk on half the wave sections.

The walk reproduces the recorded path exactly on some sections and diverges by tens of units on
others. Both candidates are discrete, mid-section events the model does not know about:
  - a GRAVITY portal, which flips the sign of dy for a given input state
  - a SIZE portal, which changes the slope between 1 and 2
This measures the truth directly: at each frame, work out the slope and the sign the recorded path
actually had, and report where either changes.
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_traj import load  # noqa: E402


def probe(path):
    o = load(path)
    if not isinstance(o, dict):
        return None
    fixes = o.get('frameFixes')
    inputs = o.get('inputs')
    if not fixes or not inputs:
        return None
    pos = {}
    for f in fixes:
        if isinstance(f, dict) and 'frame' in f:
            p1 = f.get('p1') or {}
            if 'x' in p1 and 'y' in p1:
                pos[f['frame']] = (p1['x'], p1['y'])
    if len(pos) < 500:
        return None

    ev = {}
    for i in inputs:
        if isinstance(i, dict) and not i.get('2p') and i.get('btn', 1) in (0, 1):
            ev[i['frame']] = bool(i.get('down'))

    frames = sorted(pos)
    held = False
    slopes = []      # (frame, signed dy/dx, held)
    for k in range(1, len(frames)):
        a, b = frames[k-1], frames[k]
        if b != a + 1:
            continue
        if a in ev:
            held = ev[a]
        x0, y0 = pos[a]; x1, y1 = pos[b]
        dx = x1 - x0
        if dx <= 0.0001:
            continue
        s = (y1 - y0) / dx
        if abs(abs(s) - 1.0) < 0.02 or abs(abs(s) - 2.0) < 0.02:
            slopes.append((a, s, held))

    if len(slopes) < 400:
        return None

    # For each frame classify: magnitude (1 or 2 => size) and whether sign agrees with held
    sizes = set()
    polarity = set()
    for fr, s, h in slopes:
        sizes.add(2 if abs(s) > 1.5 else 1)
        # normal orientation: held => +, released => -
        polarity.add(1 if ((s > 0) == h) else -1)
    return {
        'file': os.path.basename(path),
        'n': len(slopes),
        'sizes': sorted(sizes),
        'polarity': sorted(polarity),
    }


files = sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'blobs', '*.gdr')))
rows = []
for f in files:
    try:
        r = probe(f)
    except Exception:
        r = None
    if r:
        rows.append(r)
    if len(rows) >= 16:
        break

print('%-32s %-7s %-12s %s' % ('file', 'frames', 'sizes seen', 'polarity seen'))
mixedSize = mixedPol = 0
for r in rows:
    if len(r['sizes']) > 1: mixedSize += 1
    if len(r['polarity']) > 1: mixedPol += 1
    print('%-32s %-7d %-12s %s' % (r['file'][:32], r['n'], r['sizes'], r['polarity']))

print()
print('sections analysed        : %d' % len(rows))
print('with a SIZE change mid-run : %d   (mini portal -> slope switches 1 <-> 2)' % mixedSize)
print('with a POLARITY flip       : %d   (gravity portal -> held no longer means up)' % mixedPol)
