"""Does integrating against the level keep the route in the level?

The bug, straight from a real log. The route the mod drew, sampled along its length:

    x=344  y=189      x=2666 y=-310     x=7309 y=1936     x=21238 y=5037

Correct at the first click, then thousands of units above a level whose geometry lives between
y=100 and y=400. Nothing above it and nothing below it, so the height fit had nothing to say in 49
of 59 windows and no amount of patching a finished polyline could reach it.

The cause is that free integration has nothing holding it. 254 transitions of +-1 and +-2, and any
imbalance between held and released compounds without limit. The real run does not do that, and the
reason is the sliding: every slide is a stretch travelled flat instead of climbing, and across the
recorded runs there are 129 of them. Sliding is not a correction to the shape - it IS the shape.

So: same inputs, same corridor, integrated both ways. Free must run away; colliding must not.
"""
import random

SLOPE, STEP, SKIN = 2.0, 10.0, 1.0


def corridor(length=22000.0, gap=90.0, base=200.0, wobble=140.0):
    """A wave corridor that meanders, as a real level's does. Walls of 30x30 blocks."""
    rnd = random.Random(7)
    boxes, x, mid = [], 0.0, base
    while x < length:
        mid += rnd.uniform(-6.0, 6.0)
        mid = max(base - wobble, min(base + wobble, mid))
        boxes.append((x, x + 30.0, mid - gap / 2 - 30.0, mid - gap / 2))
        boxes.append((x, x + 30.0, mid + gap / 2, mid + gap / 2 + 30.0))
        x += 30.0
    return sorted(boxes), base


def transitions(length=22000.0, n=254, seed=3, bias=0.55):
    """Press/release points with a slight imbalance - which is all it takes."""
    rnd = random.Random(seed)
    xs = sorted(rnd.uniform(300.0, length - 300.0) for _ in range(n))
    return [(x, 1 if rnd.random() < bias else 0) for x in xs]


def slide_to(x_prev, x, from_y, to_y, boxes, slopes=()):
    """Port of slideTo in src/main.cpp: identify the floor and ceiling, then stay between them."""
    TOL = 20.0
    f0, c0 = -1e18, 1e18
    for x0, x1, y0, y1 in boxes:
        if x0 > x_prev:
            break
        if x1 < x_prev:
            continue
        if y1 <= from_y:
            f0 = max(f0, y1)
        elif y0 >= from_y:
            c0 = min(c0, y0)
        elif y1 - from_y <= from_y - y0:
            f0 = max(f0, y1)
        else:
            c0 = min(c0, y0)
    for sx0, sx1, ya, yb in slopes:
        if sx0 > x_prev:
            break
        if sx1 < x_prev or sx1 - sx0 < 1e-6:
            continue
        v = ya + (yb - ya) * ((x_prev - sx0) / (sx1 - sx0))
        if v <= from_y:
            f0 = max(f0, v)
        else:
            c0 = min(c0, v)

    band = {'below': -1e18, 'above': 1e18}

    def place(v):
        if f0 > -1e17 and abs(v - f0) <= TOL:
            is_floor = True
        elif c0 < 1e17 and abs(v - c0) <= TOL:
            is_floor = False
        else:
            is_floor = v <= from_y
        if is_floor:
            band['below'] = max(band['below'], v)
        else:
            band['above'] = min(band['above'], v)

    for x0, x1, y0, y1 in boxes:
        if x0 > x:
            break
        if x1 < x:
            continue
        place(y1)
        place(y0)
    for sx0, sx1, ya, yb in slopes:
        if sx0 > x:
            break
        if sx1 < x or sx1 - sx0 < 1e-6:
            continue
        place(ya + (yb - ya) * ((x - sx0) / (sx1 - sx0)))

    lo = band['below'] + SKIN if band['below'] > -1e17 else -1e18
    hi = band['above'] - SKIN if band['above'] < 1e17 else 1e18
    if lo > hi:
        return (lo + hi) * 0.5
    return max(lo, min(hi, to_y))


def integrate(tr, boxes, y0, length, collide, slopes=()):
    pts, cx, cy, held, ti = [(0.0, y0)], 0.0, y0, False, 0
    while cx < length:
        nx = tr[ti][0] if ti < len(tr) else length
        nx = min(nx, length)
        while nx > cx:
            dx = min(nx - cx, STEP) if collide else nx - cx
            tx = cx + dx
            ny = cy + (1.0 if held else -1.0) * SLOPE * dx
            if collide:
                ny = slide_to(cx, tx, cy, ny, boxes, slopes)
            cx, cy = tx, ny
            pts.append((cx, cy))
        if cx >= length:
            break
        held = tr[ti][1] != 0
        ti += 1
    return pts


boxes, base = corridor()
tr = transitions()
LENGTH = 22000.0
ok = True


def check(name, good, detail=''):
    global ok
    print('%-52s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


free = integrate(tr, boxes, base, LENGTH, collide=False)
sim = integrate(tr, boxes, base, LENGTH, collide=True)

fmax = max(abs(p[1] - base) for p in free)
smax = max(abs(p[1] - base) for p in sim)
check('free integration runs away, as the log showed', fmax > 1500.0,
      'worst excursion %.0f units from the corridor' % fmax)
check('colliding integration stays with the level', smax < 400.0,
      'worst excursion %.0f units (free was %.0f)' % (smax, fmax))

# it must stay in the corridor, not merely bounded
def buried(pts):
    n, x = 0, 0.0
    while x <= LENGTH:
        lo, hi = 0, len(pts) - 1
        while hi - lo > 1:
            m = (lo + hi) // 2
            if pts[m][0] <= x:
                lo = m
            else:
                hi = m
        x0, x1 = pts[lo][0], pts[lo + 1][0]
        t = 0.0 if x1 - x0 < 1e-9 else (x - x0) / (x1 - x0)
        y = pts[lo][1] + (pts[lo + 1][1] - pts[lo][1]) * t
        for bx0, bx1, by0, by1 in boxes:
            if bx0 <= x <= bx1 and by0 < y < by1:
                n += 1
                break
        x += STEP
    return n


# Judged on the route's own points. A uniform grid also catches the line cutting the corner of a
# floor that steps 6 units per block between two samples 10 apart - real, and about a unit deep, so
# it is measured separately rather than counted as passing through a wall.
inside = 0
for (x, y) in sim:
    for bx0, bx1, by0, by1 in boxes:
        if bx0 > x:
            break
        if bx1 < x:
            continue
        if by0 < y < by1:
            inside += 1
            break
check('and it does not pass through the walls', inside == 0,
      '%d of %d route points inside a wall' % (inside, len(sim)))

deep, worst = 0, 0.0
x = 0.0
while x <= LENGTH:
    lo, hi = 0, len(sim) - 1
    while hi - lo > 1:
        m = (lo + hi) // 2
        if sim[m][0] <= x:
            lo = m
        else:
            hi = m
    x0, x1 = sim[lo][0], sim[lo + 1][0]
    t = 0.0 if x1 - x0 < 1e-9 else (x - x0) / (x1 - x0)
    y = sim[lo][1] + (sim[lo + 1][1] - sim[lo][1]) * t
    for bx0, bx1, by0, by1 in boxes:
        if bx0 > x:
            break
        if bx1 < x:
            continue
        if by0 < y < by1:
            d = min(y - by0, by1 - y)
            worst = max(worst, d)
            if d > 8.0:
                deep += 1
            break
    x += STEP
check('corners cut between samples stay negligible', deep == 0,
      'worst %.1f units, none over 8' % worst)

# the flat stretches ARE the slides - and they should look like the recorded ones
flats = 0
i = 0
while i < len(sim) - 1:
    if abs(sim[i + 1][1] - sim[i][1]) < 0.01 and sim[i + 1][0] > sim[i][0]:
        j = i
        while j < len(sim) - 1 and abs(sim[j + 1][1] - sim[j][1]) < 0.01:
            j += 1
        if sim[j][0] - sim[i][0] >= 20.0:
            flats += 1
        i = j
    i += 1
check('it produces flat runs - the slides themselves', flats > 20,
      '%d flat stretches of 20+ units' % flats)

# with no geometry at all it must reduce to the plain integration
def at(pts, qx):
    lo, hi = 0, len(pts) - 1
    while hi - lo > 1:
        m = (lo + hi) // 2
        if pts[m][0] <= qx:
            lo = m
        else:
            hi = m
    x0, x1 = pts[lo][0], pts[lo + 1][0]
    t = 0.0 if x1 - x0 < 1e-9 else (qx - x0) / (x1 - x0)
    return pts[lo][1] + (pts[lo + 1][1] - pts[lo][1]) * t


e1 = integrate(tr, [], base, LENGTH, collide=True)
e2 = integrate(tr, [], base, LENGTH, collide=False)
worst = max(abs(at(e1, x) - at(e2, x)) for x in range(0, int(LENGTH), 37))
check('with no geometry it is the plain integration', worst < 1e-6,
      'worst difference %.2e units' % worst)

# --- a corridor made of RAMPS, which is what the level in the screenshot actually is -------------
# The probe counted 20 slopes against 1 box at the route's first click. A route that collides only
# with boxes falls straight through a level built this way, which is exactly what "it worked for the
# first D block but went through the second" looks like.
def ramp_corridor(length=22000.0, gap=90.0, base=200.0, seg=60.0):
    rnd = random.Random(11)
    slopes, x, y = [], 0.0, base
    while x < length:
        ny = max(base - 130.0, min(base + 130.0, y + rnd.uniform(-25.0, 25.0)))
        slopes.append((x, x + seg, y - gap / 2, ny - gap / 2))
        slopes.append((x, x + seg, y + gap / 2, ny + gap / 2))
        x += seg
        y = ny
    return sorted(slopes), base


ramps, rbase = ramp_corridor()
rtr = transitions()
ramp_free = integrate(rtr, [], rbase, LENGTH, True, ())          # boxes only == no geometry here
ramp_sim = integrate(rtr, [], rbase, LENGTH, True, ramps)
rf = max(abs(p[1] - rbase) for p in ramp_free)
rs = max(abs(p[1] - rbase) for p in ramp_sim)
check('boxes-only falls through a level built of ramps', rf > 1500.0,
      'worst excursion %.0f units' % rf)
check('with slopes it stays in the ramp corridor', rs < 400.0,
      'worst excursion %.0f units (boxes-only was %.0f)' % (rs, rf))

crossed = 0
for (x, y) in ramp_sim:
    for sx0, sx1, ya, yb in ramps:
        if sx0 > x:
            break
        if sx1 < x:
            continue
        face = ya + (yb - ya) * ((x - sx0) / (sx1 - sx0))
        # it comes to rest one skin clear of the surface, not welded to it
        if abs(abs(y - face) - SKIN) < 0.05:
            crossed += 1
            break
check('and it comes to rest ON the ramps', crossed > 100,
      '%d points resting on a ramp surface' % crossed)

# --- a route inside a solid must be pushed OUT of it, never pinned in the middle ------------------
# A slab wrongly added as a ceiling trapped the route dead centre inside it for 97,000 units,
# because recording both faces makes it a floor above and a ceiling below at the same time and the
# only height satisfying both is the middle of the solid.
slab = [(x, x + 480.0, 389.0, 689.0) for x in [i * 480.0 for i in range(20)]]
cy, moved, inside = 539.0, 0.0, 0
for i in range(400):
    cx, tx = i * 10.0, i * 10.0 + 10.0
    ny = slide_to(cx, tx, cy, cy - 20.0, slab)     # trying to fall out of the bottom
    moved += abs(ny - cy)
    cy = ny
    if 389.0 < cy < 689.0:
        inside += 1
check('a route inside a solid is pushed out, not pinned in it', inside < 40 and moved > 100.0,
      '%d of 400 steps still inside, total movement %.0f units' % (inside, moved))

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
