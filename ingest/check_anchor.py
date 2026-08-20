"""When the route falls out of the level, can it find its way back in?

Measured on a real level: a route slipped through a gap in the corridor floor at about x=19,000 and
free-fell to y=-11,043 by the end of a 97,000-unit level, drawn confidently the whole way. Every
level has gaps in its floor - a route even slightly off finds one eventually.

The recovery reads the corridor off the level rather than guessing it. At one x the solid boxes give
filled intervals outright and each slope surface splits a band in two, so merging them leaves the
free bands exactly; a wave corridor is one bounded on both sides and roughly player-sized. This
checks that the band it picks is the corridor, that it only fires when the route really is lost, and
that it never picks the open sky above the level - which fits everything and therefore means nothing.
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MIN_BAND, MAX_BAND = 40.0, 260.0


def reanchor_at(x, ref_y, boxes, slopes=()):
    """Port of reanchorAt in src/main.cpp."""
    iv = []
    for x0, x1, y0, y1 in boxes:
        if x0 > x:
            break
        if x1 < x:
            continue
        iv.append((y0, y1))
    for sx0, sx1, ya, yb in slopes:
        if sx0 > x:
            break
        if sx1 < x or sx1 - sx0 < 1e-6:
            continue
        sy = ya + (yb - ya) * ((x - sx0) / (sx1 - sx0))
        iv.append((sy, sy))
    if len(iv) < 2:
        return None
    iv.sort()
    merged = []
    for a, b in iv:
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    best, best_d = None, 1e18
    for k in range(len(merged) - 1):
        lo, hi = merged[k][1], merged[k + 1][0]
        h = hi - lo
        if h < MIN_BAND or h > MAX_BAND:
            continue
        c = (lo + hi) * 0.5
        d = abs(c - ref_y)
        if d < best_d:
            best_d, best = d, c
    return best


def corridor(length=8000.0, gap=90.0, base=200.0, hole=None, ground=True):
    """A wave corridor. `hole` removes the floor over an x range, as every real level does."""
    boxes, x = [], 0.0
    while x < length:
        in_hole = hole is not None and hole[0] <= x < hole[1]
        if not in_hole:
            boxes.append((x, x + 30.0, base - gap / 2 - 30.0, base - gap / 2))
        boxes.append((x, x + 30.0, base + gap / 2, base + gap / 2 + 30.0))
        if ground:                      # the level's own floor, far below the corridor
            boxes.append((x, x + 30.0, base - 900.0, base - 870.0))
        x += 30.0
    return sorted(boxes)


ok = True


def check(name, good, detail=''):
    global ok
    print('%-54s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


BASE, GAP = 200.0, 90.0

# --- 1. it finds the corridor, not the void it fell into -----------------------------------------
boxes = corridor()
r = reanchor_at(4000.0, BASE, boxes)
check('finds the corridor from inside it', r is not None and abs(r - BASE) < 1.0,
      'returned %s, corridor centre is %.0f' % ('None' if r is None else '%.1f' % r, BASE))

# --- 2. a route that has fallen 1000 units below still finds it ----------------------------------
r = reanchor_at(4000.0, BASE, boxes)      # refY is the LAST GOOD height, not where it fell to
check('a lost route is put back where it was last seen', r is not None and abs(r - BASE) < 1.0,
      'returned %s' % ('None' if r is None else '%.1f' % r))

# --- 3. the open sky above the level is never chosen ---------------------------------------------
# Above the ceiling there is nothing at all, so that band is unbounded and must not qualify.
r = reanchor_at(4000.0, BASE + 5000.0, boxes)
above_ceiling = BASE + GAP / 2 + 30.0
check('open sky above the level is not a candidate',
      r is None or r < above_ceiling,
      'returned %s, ceiling top is %.0f' % ('None' if r is None else '%.1f' % r, above_ceiling))

# --- 4. it prefers the corridor nearest where the route was last supported ------------------------
# Two corridors stacked: the run was in the upper one, so that is the one to return to.
two = []
for x in [i * 30.0 for i in range(int(8000 / 30))]:
    for mid in (200.0, 700.0):
        two.append((x, x + 30.0, mid - GAP / 2 - 30.0, mid - GAP / 2))
        two.append((x, x + 30.0, mid + GAP / 2, mid + GAP / 2 + 30.0))
two.sort()
check('picks the corridor nearest the last good height (upper)',
      abs(reanchor_at(4000.0, 700.0, two) - 700.0) < 1.0)
check('picks the corridor nearest the last good height (lower)',
      abs(reanchor_at(4000.0, 200.0, two) - 200.0) < 1.0)

# --- 5. nothing corridor-shaped -> it declines rather than inventing one --------------------------
sparse = [(0.0, 30.0, 0.0, 30.0)]
check('declines when there is no corridor to return to', reanchor_at(15.0, 200.0, sparse) is None)

# a band far too tall is open level, not a corridor
wide = [(0.0, 30.0, 0.0, 30.0), (0.0, 30.0, 900.0, 930.0)]
check('a 870-unit gap is open level, not a corridor', reanchor_at(15.0, 400.0, wide) is None)

# --- 6. end to end: a route through a floor gap recovers ------------------------------------------
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'check_sim.py')).read()
g = {}
exec(src.split('boxes, base = corridor()')[0], g)
slide_to = g['slide_to']

# No ground layer: on the real level the probe read '^1010 v-' - nothing below the
# route at all. A level that does have a ground far below catches a falling route and
# it is merely in the wrong place, which is what the trust gate is for.
holed = corridor(hole=(3000.0, 3400.0), ground=False)
STEP, SLOPE = 10.0, 2.0
for recover in (False, True):
    cx, cy, held = 0.0, BASE, False
    last_good, adrift, anchors = BASE, 0, 0
    lo_seen = BASE
    while cx < 8000.0:
        tx = cx + STEP
        ny = cy + (1.0 if held else -1.0) * SLOPE * STEP
        ny = slide_to(cx, tx, cy, ny, holed)
        # supported means SANDWICHED - a floor within 900 below AND a ceiling within 900 above.
        # Either one alone is satisfied by the underside of the very floor it fell through.
        near = [b for b in holed if b[0] <= tx <= b[1]]
        f0 = max([b[3] for b in near if b[3] <= cy], default=None)
        c0 = min([b[2] for b in near if b[2] >= cy], default=None)
        sup = (f0 is not None and cy - f0 < 900.0) and (c0 is not None and c0 - cy < 900.0)
        if sup:
            adrift, last_good = 0, ny
        else:
            adrift += 1
        if recover and adrift > 90:
            r = reanchor_at(tx, last_good, holed)
            if r is not None:
                ny, adrift, anchors = r, 0, anchors + 1
        cx, cy = tx, ny
        lo_seen = min(lo_seen, cy)
        if int(cx) % 400 == 0:
            held = not held
    if recover:
        check('a route through a floor gap gets back into the corridor',
              anchors >= 1 and lo_seen > BASE - 900.0,
              '%d recoveries, lowest point %.0f' % (anchors, lo_seen))
    else:
        check('without recovery it drops out of the level', lo_seen < BASE - 200.0,
              'lowest point %.0f' % lo_seen)

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
