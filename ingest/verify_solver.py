"""Verify the trajectory SOLVER (event construction + walk) against recorded truth.

The mod builds a zigzag from press/release events and integrates dy/dx between them. The last
version latched `held` on and drew one straight line, because an action whose release equalled its
press advanced the cursor without ever releasing.

This reproduces the exact solver the mod runs - build events, sort, walk - and scores it against the
per-frame positions recorded inside the .gdr files. A correct solver reproduces the real path; a
solver that fails to release produces a straight line and a huge error.
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_traj import load  # noqa: E402


def solve(path):
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
    if len(pos) < 800:
        return None

    frames = sorted(pos)

    # Find the longest continuous wave stretch with a CONSTANT slope magnitude and no polarity
    # change, so the test isolates the solver rather than the portal handling.
    runs = []
    cur = None
    prevs = None
    for k in range(1, len(frames)):
        a, b = frames[k-1], frames[k]
        if b != a + 1:
            cur = None; prevs = None; continue
        x0, y0 = pos[a]; x1, y1 = pos[b]
        dx = x1 - x0
        if dx <= 0.0001:
            cur = None; prevs = None; continue
        s = (y1 - y0) / dx
        mag = abs(s)
        if not (abs(mag - 1.0) < 0.02 or abs(mag - 2.0) < 0.02):
            cur = None; prevs = None; continue
        key = round(mag)
        if cur is None or key != prevs:
            cur = [a, b, key]; runs.append(cur); prevs = key
        else:
            cur[1] = b
    runs = [r for r in runs if r[1] - r[0] >= 600]
    if not runs:
        return None
    f0, f1, slope = max(runs, key=lambda r: r[1] - r[0])

    # EVENT LIST - exactly what the mod now builds
    ev = []
    for i in inputs:
        if not isinstance(i, dict) or i.get('2p') or i.get('btn', 1) not in (0, 1):
            continue
        ev.append((i['frame'], bool(i.get('down'))))
    ev.sort()

    # held at f0
    held = False
    for fr, dn in ev:
        if fr > f0:
            break
        held = dn

    # determine polarity empirically over the first few frames (stands in for the gravity flip
    # the mod reads live from the player)
    x0, y0 = pos[f0]; x1, y1 = pos[f0 + 1]
    obs = (y1 - y0) / (x1 - x0)
    pol = 1.0 if ((obs > 0) == held) else -1.0

    cy = y0; cx = x0
    h = held
    ei = 0
    while ei < len(ev) and ev[ei][0] <= f0:
        ei += 1
    errs = []
    flips = 0
    for fr in range(f0 + 1, f1 + 1):
        while ei < len(ev) and ev[ei][0] <= fr:
            tx = pos.get(ev[ei][0], (None, None))[0]
            if tx is not None and tx > cx:
                cy += (1.0 if h else -1.0) * pol * slope * (tx - cx)
                cx = tx
            if h != ev[ei][1]:
                flips += 1
            h = ev[ei][1]
            ei += 1
        px, ptrue = pos[fr]
        if px > cx:
            cy += (1.0 if h else -1.0) * pol * slope * (px - cx)
            cx = px
        errs.append(abs(cy - ptrue))

    if not errs:
        return None
    errs.sort()
    return {
        'file': os.path.basename(path)[:26],
        'frames': f1 - f0,
        'slope': slope,
        'flips': flips,
        'median': errs[len(errs) // 2],
        'p95': errs[int(len(errs) * 0.95)],
        'max': errs[-1],
    }


files = sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'blobs', '*.gdr')))
rows = []
for f in files:
    try:
        r = solve(f)
    except Exception:
        r = None
    if r:
        rows.append(r)
    if len(rows) >= 14:
        break

print('%-28s %-7s %-6s %-7s %-9s %-9s %s'
      % ('file', 'frames', 'slope', 'flips', 'median', 'p95', 'max'))
for r in rows:
    print('%-28s %-7d %-6d %-7d %-9.3f %-9.3f %.3f'
          % (r['file'], r['frames'], r['slope'], r['flips'], r['median'], r['p95'], r['max']))

if rows:
    med = sorted(r['median'] for r in rows)[len(rows) // 2]
    zero = sum(1 for r in rows if r['flips'] == 0)
    print('\nsections %d   median error %.3f units   sections with NO flips (straight line): %d'
          % (len(rows), med, zero))
    print('A working solver: median well under 1 unit, and flips >> 0 on every section.')
