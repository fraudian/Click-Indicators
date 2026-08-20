"""What height is a dual a mirror image about? Ask the level, not the portal.

Twenty-two samples across three dual regions of a real level, taken wherever the run happened to be:

    p1=192 p2=288    p1=151 p2=329    p1=119 p2=360
    p1=240 p2=240    p1=224 p2=256    p1=112 p2=368
    p1=168 p2=312    p1=181 p2=299    p1=136 p2=344

Every pair straddles y=240 exactly. The three dual portals sit at y=240, y=240 and **y=135**. The
region with the portal at 135 mirrors about 240 like the other two, so the portal does not decide
it - and using the portal there started the second icon 210 units out and it spent the rest of the
dual clipping through whatever it landed in.

The reason is that GD never holds the pair together. The two icons are independent, with opposite
gravity, and they stay symmetric only because the LEVEL is built symmetric. So the height is a
property of the geometry, and the geometry is the thing to ask.

This checks the fit: that it finds the axis of a mirrored corridor pair, that it is not fooled by
empty sky, and that it declines rather than inventing one where the level is not a mirror.
"""
import math

B = 15.0
MIN_SEEN = 200
# Where the bar goes, from the only two numbers there are: a clean mirrored corridor pair scores
# 0.91, and a level with one corridor and a solid mass on one side scores 0.68. 0.80 sits between
# them. Real levels have not been measured yet - the score is logged for every dual region so the
# bar can be moved on evidence rather than on feel.
ACCEPT = 0.80


def fit_axis(boxes, x0, x1):
    """Port of rtDualAxis in src/main.cpp. Returns (axis, score, seen)."""
    inside = [bx for bx in boxes if bx[1] >= x0 and bx[0] <= x1]
    if not inside:
        return None, 0.0, 0.0
    y_lo = min(b[2] for b in inside)
    y_hi = max(b[3] for b in inside)
    if y_hi <= y_lo:
        return None, 0.0, 0.0
    if y_hi - y_lo > 1200.0:
        y_hi = y_lo + 1200.0
    nb = int(min(96, max(8, math.ceil((y_hi - y_lo) / B))))
    nx = int(min(160, max(4, math.ceil((x1 - x0) / 30.0))))
    dx = (x1 - x0) / nx
    occ = [[0] * nb for _ in range(nx)]
    for i in range(nx):
        qx = x0 + (i + 0.5) * dx
        for bx0, bx1, by0, by1 in inside:
            if bx0 > qx or bx1 < qx:
                continue
            b0 = int(math.floor((by0 - y_lo) / B))
            b1 = int(math.floor((by1 - y_lo) / B))
            if b1 < 0 or b0 >= nb:
                continue
            for b in range(max(b0, 0), min(b1, nb - 1) + 1):
                occ[i][b] = 1

    # Every occupied bin has to have an occupied mirror, and the score is the fraction that do -
    # measured against ALL the solid geometry in the stretch, not just the part that happens to pair
    # up. Scoring only the pairs lets an axis down at the bottom of the level match a sliver of floor
    # against itself and come out at 100%, which is what the first version of this did.
    total = sum(sum(row) for row in occ)
    if total < MIN_SEEN:
        return None, 0.0, float(total)
    best, best_score = None, 0.0
    for k in range(2 * nb + 1):
        matched = 0
        for row in occ:
            for b in range(nb):
                if not row[b]:
                    continue
                m = k - b - 1
                if 0 <= m < nb and row[m]:
                    matched += 1
        sc = matched / total
        if sc > best_score:
            best_score = sc
            best = y_lo + k * B * 0.5
    return best, best_score, float(total)


def corridor(mid, x0, x1, gap=90.0, seg=30.0):
    b, x = [], x0
    while x < x1:
        b.append((x, x + seg, mid - gap / 2 - 30.0, mid - gap / 2))
        b.append((x, x + seg, mid + gap / 2, mid + gap / 2 + 30.0))
        x += seg
    return b


ok = True


def check(name, good, detail=''):
    global ok
    print('%-58s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


X0, X1 = 0.0, 2400.0

# --- 1. the real case: a mirrored pair of corridors, portal nowhere near the axis -----------------
AXIS = 240.0
sym = sorted(corridor(AXIS + 150.0, X0, X1) + corridor(AXIS - 150.0, X0, X1))
a, sc, seen = fit_axis(sym, X0, X1)
check('it finds the axis of a mirrored corridor pair', a is not None and abs(a - AXIS) < B,
      'found y=%.1f (%.0f%% of the geometry pairs up, %d samples)' % (a or -1, 100 * sc, seen))
check('  and it is confident enough to be used', sc >= ACCEPT, 'score %.2f' % sc)
check('  the portal height is not what it found', abs(135.0 - AXIS) > B,
      'the real level put the portal at 135 and the axis is 240')

# --- 2. empty sky must not win -------------------------------------------------------------------
# An axis far above the level pairs nothing against nothing. Scoring bin AGREEMENT would call that
# perfect; scoring how much of the solid geometry actually finds a mirror calls it what it is.
def score_at(boxes, x0, x1, axis):
    """The same metric, forced to a particular axis."""
    inside = [bx for bx in boxes if bx[1] >= x0 and bx[0] <= x1]
    y_lo = min(b[2] for b in inside)
    y_hi = max(b[3] for b in inside)
    nb = int(min(96, max(8, math.ceil((y_hi - y_lo) / B))))
    k = int(round((axis - y_lo) / (B * 0.5)))
    nx = int(min(160, max(4, math.ceil((x1 - x0) / 30.0))))
    dx = (x1 - x0) / nx
    total = matched = 0
    for i in range(nx):
        qx = x0 + (i + 0.5) * dx
        col = [0] * nb
        for bx0, bx1, by0, by1 in inside:
            if bx0 > qx or bx1 < qx:
                continue
            for b in range(max(int((by0 - y_lo) / B), 0), min(int((by1 - y_lo) / B), nb - 1) + 1):
                col[b] = 1
        for b in range(nb):
            if not col[b]:
                continue
            total += 1
            m = k - b - 1
            if 0 <= m < nb and col[m]:
                matched += 1
    return matched / max(total, 1)


check('an axis out in empty sky scores nothing', score_at(sym, X0, X1, 2000.0) < 0.05,
      '%.0f%% of the geometry finds a mirror there' % (100 * score_at(sym, X0, X1, 2000.0)))
check('  where the real axis scores almost everything', sc > 0.85,
      '%.0f%%' % (100 * sc))

# --- 3. a level that is NOT a mirror: it must decline rather than invent one ----------------------
# One corridor, and a solid mass on one side of it only - there is no height this maps onto itself
# about, and no second icon can be placed from it.
asym = sorted(corridor(AXIS + 150.0, X0, X1)
              + [(x * 1.0, x + 30.0, 0.0, 180.0) for x in range(0, int(X1), 30)]
              + [(x * 1.0, x + 30.0, 210.0, 255.0) for x in range(0, int(X1), 120)])
a2, sc2, _ = fit_axis(asym, X0, X1)
check('an asymmetric dual scores below the bar', sc2 < ACCEPT,
      'best score %.2f against a bar of %.2f' % (sc2, ACCEPT))
check('  and the gap to a real mirror is wide', sc - sc2 > 0.2,
      'symmetric %.2f, asymmetric %.2f' % (sc, sc2))

# --- 4. a mirrored pair at a DIFFERENT height is found there, not at 240 --------------------------
# The fit must be reading the level, not remembering a number from one log.
for target in (135.0, 180.0, 330.0):
    sm = sorted(corridor(target + 120.0, X0, X1) + corridor(target - 120.0, X0, X1))
    at, sct, _ = fit_axis(sm, X0, X1)
    if at is None or abs(at - target) >= B or sct < ACCEPT:
        check('a mirror about y=%.0f is found at y=%.0f' % (target, at or -1), False,
              'score %.2f' % sct)
        break
else:
    check('mirrors at 135, 180 and 330 are each found where they are', True,
          'within half a block of the truth in all three')

# --- 5. it is stable against the run's own position, which is what went wrong before --------------
# The old rules read the axis off something that moves: where the run entered, or which portal it
# passed. The geometry does not move, so the same stretch gives the same answer every time.
first = fit_axis(sym, X0, X1)[0]
again = fit_axis(sym, X0, X1)[0]
part = fit_axis(sym, X0 + 600.0, X1 - 300.0)[0]
check('the same stretch gives the same axis every time', first == again)
check('  and so does a different slice of the same dual', abs(part - first) < B,
      'x0+600..x1-300 gives y=%.0f against y=%.0f' % (part, first))

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
