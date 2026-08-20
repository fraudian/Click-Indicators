"""Does the slide clamp preserve the route where nothing is in the way?

The clamp samples every segment finely, pushes samples out of solid boxes, then collapses what did
not move. Two things have to hold or the route is worse off than before it:

  1. A route that touches no geometry must come out EXACTLY as it went in - same points, same
     button flags. If sampling alone perturbs the line, every level pays for a feature that only
     4% of wave frames need.
  2. pts and hold must stay paired (len(hold) == len(pts) - 1) and x must stay ascending, in every
     case. A hold array off by one draws a line across the cube section between two wave sections -
     the exact class of bug that has broken this feature before.

This is a port of the C++ in rtSolve, run against synthetic geometry, because the real geometry
lives inside Geometry Dash and cannot be read offline.
"""
import random

STEP, SKIN, MAXSTEP, MAXOFF = 10.0, 1.0, 34.0, 60.0


def interp(poly, qx):
    if qx <= poly[0][0]:
        return poly[0][1]
    if qx >= poly[-1][0]:
        return poly[-1][1]
    lo, hi = 0, len(poly) - 1
    while hi - lo > 1:
        m = (lo + hi) // 2
        if poly[m][0] <= qx:
            lo = m
        else:
            hi = m
    x0, x1 = poly[lo][0], poly[lo + 1][0]
    if x1 - x0 < 1e-9:
        return poly[lo][1]
    t = (qx - x0) / (x1 - x0)
    return poly[lo][1] + (poly[lo + 1][1] - poly[lo][1]) * t


def count_buried(poly, boxes):
    """How much of the drawn LINE is inside geometry, on a fixed grid - not at the samples, because
    the collapse joins two kept points straight and that join can cut a corner no sample was in."""
    if len(poly) < 2:
        return 0
    n, x = 0, poly[0][0]
    while x <= poly[-1][0]:
        y = interp(poly, x)
        for x0, x1, y0, y1 in boxes:
            if x0 <= x <= x1 and y0 < y < y1:
                n += 1
                break
        x += STEP
    return n


def clamp_route(pts, hold, boxes):
    """Port of the clamp in src/main.cpp. boxes: list of (x0, x1, y0, y1), sorted by x0."""
    maxw = max((b[1] - b[0] for b in boxes), default=0.0)
    state = {'lo': 0, 'slid': 0, 'refused': 0, 'burRaw': 0, 'burNew': 0, 'off': 0.0}
    orig_pts, orig_hold = list(pts), list(hold)

    def inside_at(vx, y):
        for j in range(state['lo'], len(boxes)):
            x0, x1, y0, y1 = boxes[j]
            if x0 > vx:
                break
            if x1 < vx:
                continue
            if y0 < y < y1:
                return True
        return False

    def place(vx, raw_y, direction):
        vy = raw_y + state['off']
        while state['lo'] < len(boxes) and boxes[state['lo']][0] < vx - maxw:
            state['lo'] += 1

        def escape(d):
            ny = vy
            for _ in range(6):
                top, bot, inside = -1e18, 1e18, False
                for j in range(state['lo'], len(boxes)):
                    x0, x1, y0, y1 = boxes[j]
                    if x0 > vx:
                        break
                    if x1 < vx:
                        continue
                    if ny < y0 or ny > y1:
                        continue
                    inside = True
                    top, bot = max(top, y1), min(bot, y0)
                if not inside:
                    return ny
                nxt = top + SKIN if d > 0 else bot - SKIN
                if not abs(nxt - ny) > 1e-9:
                    break
                if abs(nxt - vy) > MAXSTEP:
                    break
                ny = nxt
            return None

        ny = escape(direction)
        if ny is None:
            ny = escape(-direction)
        ok = ny is not None and abs(state['off'] + (ny - vy)) <= MAXOFF
        if not ok:
            state['refused'] += 1
            return vy
        if not abs(ny - vy) > 1e-9:
            return vy
        state['off'] += (ny - vy)
        state['slid'] += 1
        return ny

    out, outhold = [], []
    last_dir = 1
    for i in range(len(pts) - 1):
        ax, ay = pts[i]
        bx, by = pts[i + 1]
        direction = 1 if by <= ay else -1
        last_dir = direction
        out.append((ax, place(ax, ay, direction)))
        span = bx - ax
        extra = int(span / STEP) if span > STEP * 1.5 else 0
        extra = min(extra, 4096)
        for k in range(1, extra + 1):
            t = k / (extra + 1)
            sx, sy = ax + span * t, ay + (by - ay) * t
            out.append((sx, place(sx, sy, direction)))
        outhold.extend([hold[i]] * (extra + 1))
    out.append((pts[-1][0], place(pts[-1][0], pts[-1][1], last_dir)))

    if len(out) > 2:
        mp, mh = [out[0]], []
        for i in range(1, len(out) - 1):
            if outhold[i - 1] == outhold[i]:
                ax, ay = mp[-1]
                sx = out[i + 1][0] - ax
                if sx > 1e-6:
                    t = (out[i][0] - ax) / sx
                    if abs(ay + (out[i + 1][1] - ay) * t - out[i][1]) < 0.05:
                        continue
            mp.append(out[i])
            mh.append(outhold[i - 1])
        mp.append(out[-1])
        mh.append(outhold[-1])
        out, outhold = mp, mh

    # do no harm, judged on the finished lines
    state['burRaw'] = count_buried(orig_pts, boxes)
    state['burNew'] = count_buried(out, boxes)
    if state['burNew'] > state['burRaw']:
        return orig_pts, orig_hold, state
    return out, outhold, state


def zigzag(seed, n=60):
    """A wave route: alternating +-1 slopes, one hold flag per segment."""
    rnd = random.Random(seed)
    x, y, held = 0.0, 300.0, False
    pts, hold = [(x, y)], []
    for _ in range(n):
        run = rnd.choice([30.0, 45.0, 60.0, 90.0, 120.0])
        y += (1.0 if held else -1.0) * run
        x += run
        pts.append((x, y))
        hold.append(1 if held else 0)
        held = not held
    return pts, hold


def check(name, ok, detail=''):
    print('%-46s %s %s' % (name, 'PASS' if ok else '** FAIL **', detail))
    return ok


allok = True

# --- 1. geometry present but far away: must be a byte-for-byte round trip ------------------------
for seed in range(12):
    pts, hold = zigzag(seed)
    far = [(float(i * 100), float(i * 100 + 30), -900.0, -860.0) for i in range(40)]
    o, h, st = clamp_route(pts, hold, far)
    same = (len(o) == len(pts) and len(h) == len(hold)
            and all(abs(a[0] - b[0]) < 1e-9 and abs(a[1] - b[1]) < 1e-9 for a, b in zip(o, pts))
            and h == hold)
    if not same:
        allok &= check('round trip, geometry out of reach (seed %d)' % seed, False,
                       'in %d pts -> out %d pts, slid=%d' % (len(pts), len(o), st['slid']))
        break
else:
    allok &= check('round trip, geometry out of reach (12 routes)', True)

# --- 2. pairing and ordering hold under real clamping --------------------------------------------
bad = None
for seed in range(40):
    pts, hold = zigzag(seed)
    rnd = random.Random(seed + 5000)
    boxes = []
    for i in range(60):
        bx = rnd.uniform(0, pts[-1][0])
        by = rnd.uniform(min(p[1] for p in pts), max(p[1] for p in pts))
        boxes.append((bx, bx + 30.0, by, by + 30.0))
    boxes.sort()
    o, h, st = clamp_route(pts, hold, boxes)
    if len(h) != len(o) - 1:
        bad = 'seed %d: %d holds for %d pts' % (seed, len(h), len(o)); break
    if any(o[i + 1][0] < o[i][0] - 1e-9 for i in range(len(o) - 1)):
        bad = 'seed %d: x went backwards' % seed; break
    if any(f not in (0, 1) for f in h):
        bad = 'seed %d: hold flag corrupted' % seed; break
allok &= check('pts/hold paired + x ascending (40 routes)', bad is None, bad or '')

# --- 3. the clamp can only ever REDUCE how much of the route is buried ----------------------------
# It cannot promise zero: a route dropped into geometry too deep to escape within one block, or one
# that would need more than three blocks of accumulated offset, is deliberately left alone rather
# than dragged somewhere invented. What it must never do is make penetration worse. Measured here
# independently of the counters the implementation keeps for itself.
rawtot, cliptot, worse = 0, 0, 0
for seed in range(40):
    pts, hold = zigzag(seed)
    rnd = random.Random(seed + 9000)
    boxes = sorted((lambda bx, by: (bx, bx + 30.0, by, by + 30.0))(
        rnd.uniform(0, pts[-1][0]),
        rnd.uniform(min(p[1] for p in pts), max(p[1] for p in pts))) for _ in range(60))
    o, _, st = clamp_route(pts, hold, boxes)
    r, c = count_buried(pts, boxes), count_buried(o, boxes)
    rawtot += r
    cliptot += c
    if c > r:
        worse += 1
allok &= check('clamp never increases penetration (40 routes)', worse == 0,
               'buried samples %d -> %d (%.0f%% less), %d routes made worse'
               % (rawtot, cliptot, 100.0 * (rawtot - cliptot) / max(rawtot, 1), worse))

# --- 4. a route buried in one huge block is refused, not dragged 500 units -----------------------
pts, hold = zigzag(3)
huge = [(-1e4, 1e4, -1e4, 1e4)]
o, h, st = clamp_route(pts, hold, huge)
allok &= check('route buried in one huge block is refused',
               st['slid'] == 0 and len(o) == len(pts),
               'slid=%d refused=%d' % (st['slid'], st['refused']))

# --- 5. the point of the whole thing: meeting a block must produce a FLAT run ---------------------
# A wave descending at -1 onto the top of a block. The measurement said real slides are gradient
# 0.0, so the clamped route has to come out flat along the face - not merely "not inside".
pts = [(0.0, 100.0), (100.0, 0.0)]          # one -1 segment
hold = [0]
block = [(40.0, 90.0, 30.0, 60.0)]          # top face at y=60, spanning x 40..90
o, h, st = clamp_route(pts, hold, block)
face = [p for p in o if 40.0 <= p[0] <= 90.0]
# Two points is what a flat run SHOULD collapse to; what matters is how far it runs.
flat = (len(face) >= 2 and all(abs(p[1] - (60.0 + SKIN)) < 1e-6 for p in face)
        and face[-1][0] - face[0][0] >= 30.0)
allok &= check('meeting a block gives a flat run along its face', flat,
               'flat for %.0f units at y=%s'
               % (face[-1][0] - face[0][0] if len(face) >= 2 else 0.0,
                  sorted({round(p[1], 1) for p in face})))

# --- 6. it leaves the block displaced, and it does NOT cliff back through it -----------------------
# The cliff is the failure mode of not carrying: flat along the face, then a near vertical drop
# straight through the block it had just been resting on.
after = [p for p in o if p[0] > 92.0]
grad = all(abs((after[i + 1][1] - after[i][1]) / (after[i + 1][0] - after[i][0]) + 1.0) < 1e-6
           for i in range(len(after) - 1))
allok &= check('it resumes its gradient past the block, displaced',
               grad and all(p[1] > (100.0 - p[0]) + 1.0 for p in after))

# --- 7. and the carry is bounded at one block, whatever the level throws at it --------------------
# 90 units of carried drift is what put the route through the floor on a corridor with 9.4 units of
# slack. Anything larger than a block is a wrong HEIGHT, which is the drift pass's job.
worst = 0.0
for seed in range(40):
    p2, h2 = zigzag(seed)
    rnd = random.Random(seed + 9000)
    bx2 = sorted((lambda bx, by: (bx, bx + 30.0, by, by + 30.0))(
        rnd.uniform(0, p2[-1][0]),
        rnd.uniform(min(q[1] for q in p2), max(q[1] for q in p2))) for _ in range(60))
    _, _, s2 = clamp_route(p2, h2, bx2)
    worst = max(worst, abs(s2['off']))
allok &= check('carried slide stays within its cap', worst <= MAXOFF + 1e-6,
               'worst carry %.1f units over 40 routes' % worst)

print()
print('ALL CHECKS PASS' if allok else 'SOMETHING IS WRONG - do not ship')
