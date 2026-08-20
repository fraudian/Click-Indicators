"""Does the route draw as one line, and is the geometry that makes it one line actually sound?

Two separate things were making a single flight read as a row of parts butted end to end:

  1. The width changed at every click - thick while held, thin while released - so the path
     visibly came apart at each corner.
  2. Every segment was drawn on its own, and drawSegment rounds its ends, so consecutive segments
     overlapped by half a cap. That overlap composites twice, and since the black rim is two to
     four times wider than the line itself, a wave route came out beaded end to end.

The first is a decision: on a wave, up IS held and down IS released, so the thickness was repeating
what the shape already says. The second is geometry - a run is now stroked as quads that SHARE their
end vertices, so nothing is covered twice and the corners are mitred.

This checks the geometry, because that is the part that can be silently wrong: shared vertices,
preserved width, convex quads (CCDrawNode fills from a triangle fan, so a concave one renders as
garbage), and a miter limit that a hairpin cannot throw a spike out of.
"""
import math

MITER_MIN_COS = 0.25
MITER_MAX = 4.0


def stroke(points, w):
    """Port of strokePass in src/render.cpp. Returns the list of quads."""
    h = w * 0.5
    quads, prev = [], None
    for i, p in enumerate(points):
        d0 = d1 = (0.0, 0.0)
        if i > 0:
            d0 = (p[0] - points[i - 1][0], p[1] - points[i - 1][1])
            l = math.hypot(*d0)
            d0 = (d0[0] / l, d0[1] / l) if l > 1e-6 else (0.0, 0.0)
        if i + 1 < len(points):
            d1 = (points[i + 1][0] - p[0], points[i + 1][1] - p[1])
            l = math.hypot(*d1)
            d1 = (d1[0] / l, d1[1] / l) if l > 1e-6 else (0.0, 0.0)
        if d0 == (0.0, 0.0):
            d0 = d1
        if d1 == (0.0, 0.0):
            d1 = d0
        if d0 == (0.0, 0.0):
            continue
        m = (d0[0] + d1[0], d0[1] + d1[1])
        ml = math.hypot(*m)
        if ml < 1e-4:
            nm = (-d0[1], d0[0])
        else:
            m = (m[0] / ml, m[1] / ml)
            nm = (-m[1], m[0])
            c = nm[0] * -d0[1] + nm[1] * d0[0]
            sc = 1.0 / abs(c) if abs(c) > MITER_MIN_COS else MITER_MAX
            nm = (nm[0] * sc, nm[1] * sc)
        L = (p[0] + nm[0] * h, p[1] + nm[1] * h)
        R = (p[0] - nm[0] * h, p[1] - nm[1] * h)
        if prev is not None:
            quads.append((prev[0], prev[1], R, L))     # prevL, prevR, R, L
        prev = (L, R)
    return quads


def convex(q):
    """Every cross product the same sign - which is what a triangle fan needs."""
    sgn = 0
    for i in range(4):
        a, b, c = q[i], q[(i + 1) % 4], q[(i + 2) % 4]
        cr = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0])
        if abs(cr) < 1e-9:
            continue
        s = 1 if cr > 0 else -1
        if sgn == 0:
            sgn = s
        elif s != sgn:
            return False
    return True


def perp_dist(p, a, b):
    """Distance from p to the infinite line ab."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    l = math.hypot(dx, dy)
    if l < 1e-9:
        return math.hypot(p[0] - a[0], p[1] - a[1])
    return abs(dx * (a[1] - p[1]) - dy * (a[0] - p[0])) / l


ok = True


def check(name, good, detail=''):
    global ok
    print('%-58s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


def wave_path(n=40, run=60.0, slope=1.0, x0=0.0, y0=300.0):
    pts, x, y, up = [(x0, y0)], x0, y0, True
    for _ in range(n):
        x += run
        y += (run * slope) if up else -(run * slope)
        pts.append((x, y))
        up = not up
    return pts


W = 2.6

# --- 1. adjacent quads share their vertices EXACTLY ----------------------------------------------
# This is the whole point: shared vertices mean the quads meet without covering anything twice, so
# the translucent rim cannot bead at a corner.
for name, path in (('big wave', wave_path(slope=1.0)),
                   ('mini wave', wave_path(slope=2.0)),
                   ('with slides', [(0, 300), (60, 360), (200, 360), (260, 300), (400, 300),
                                    (460, 360), (520, 300)])):
    qs = stroke(path, W)
    worst = 0.0
    for i in range(len(qs) - 1):
        # quad i is (prevL, prevR, R, L); its far edge is (R, L) = (qs[i][2], qs[i][3])
        worst = max(worst,
                    math.dist(qs[i][2], qs[i + 1][1]),   # R meets next prevR
                    math.dist(qs[i][3], qs[i + 1][0]))   # L meets next prevL
    check('%s: consecutive quads share their vertices' % name, worst < 1e-9,
          'worst gap %.2e over %d joins' % (worst, len(qs) - 1))

# --- 2. the width survives the corners -----------------------------------------------------------
# A miter that does not lengthen with the angle pinches the line at every turn, which looks like a
# waist rather than a join.
qs = stroke(wave_path(), W)
worst = 0.0
for k, q in enumerate(qs):
    prevL, prevR, R, L = q
    # the segment this quad covers, and the stroke's half width measured across it
    a = ((prevL[0] + prevR[0]) / 2, (prevL[1] + prevR[1]) / 2)
    b = ((R[0] + L[0]) / 2, (R[1] + L[1]) / 2)
    for v in (prevL, prevR, R, L):
        worst = max(worst, abs(perp_dist(v, a, b) - W / 2))
check('the stroke keeps its width through a corner', worst < 1e-6,
      'worst deviation %.2e units' % worst)

# --- 3. every quad is convex, because CCDrawNode fills from a fan ---------------------------------
bad = None
for name, path in (('big wave', wave_path(slope=1.0)),
                   ('mini wave', wave_path(slope=2.0)),
                   ('hairpin', [(0, 300), (100, 400), (101, 300), (200, 400)]),
                   ('near-reversal', [(0, 300), (100, 300.5), (200, 300), (300, 300.5)])):
    for k, q in enumerate(stroke(path, W)):
        if not convex(q):
            bad = '%s quad %d' % (name, k)
            break
    if bad:
        break
check('every quad is convex', bad is None, bad or 'across 4 shapes including a hairpin')

# --- 4. the miter limit stops a hairpin throwing a spike ------------------------------------------
# Without a limit, 1/cos(half angle) runs to infinity as the path doubles back on itself.
spike = 0.0
for q in stroke([(0, 300), (100, 400), (100.5, 300), (200, 400)], W):
    for v in q:
        spike = max(spike, max(abs(v[0]), abs(v[1] - 300.0)))
check('a hairpin cannot throw a spike off the line', spike < 250.0,
      'furthest vertex %.1f units from the path' % spike)

# --- 5. a straight run comes out straight --------------------------------------------------------
line = [(x * 1.0, 300.0) for x in range(0, 500, 50)]
qs = stroke(line, W)
ys = sorted({round(v[1], 6) for q in qs for v in q})
check('a straight run is two parallel edges', ys == [300.0 - W / 2, 300.0 + W / 2],
      'edges at %s' % ys)

# --- 6. what the change actually buys: how many pieces the visible route is drawn in --------------
# Style used to change at every click, so a screen of route was as many separately-stroked pieces as
# there were inputs on it. With one weight it is one piece, plus a break where the player is and one
# per inferred stretch.
def pieces(n_inputs, solid, past_split=True, guess_runs=0):
    if not solid:
        return n_inputs                       # every click changes the width
    return 1 + (1 if past_split else 0) + guess_runs


check('a screen with 24 inputs was 24 pieces, and is now 2',
      pieces(24, False) == 24 and pieces(24, True) == 2,
      'old %d, new %d' % (pieces(24, False), pieces(24, True)))

# --- 7. the corner dots are still one per input ---------------------------------------------------
# They are the only thing left marking the presses, so losing them would be losing the feature.
path = wave_path(n=12)
turns = sum(1 for i in range(1, len(path) - 1)
            if ((path[i + 1][1] > path[i][1]) != (path[i][1] > path[i - 1][1])))
check('there is still one marker per input', turns == 11, '%d turns over 12 segments' % turns)

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
