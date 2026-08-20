"""Can a windowed re-fit follow a corridor that a single height cannot?

That is the claim the drift pass rests on, and the log says the problem is real: over 2400 units of
the level in the screenshot the forbidden intervals merge into one band covering every offset from
-196 to +276, so NO height fits the rigid zigzag; over 1200 units there is one gap, 9.4 units wide.

Modelled here with a corridor whose walls climb steadily - which is what a run that keeps sliding
looks like relative to a shape that never does. A rigid zigzag threaded through it must fail
globally and succeed window by window, or the drift pass is not worth having.

The fitter is the same algorithm as rtFitShift, boxes only: for a segment spanning [ymin,ymax] over
its overlap with a block [by0,by1], the shifts that collide are exactly
    by0 - ymax - pad  <=  d  <=  by1 - ymin + pad
so the feasible set is the complement of a union of intervals, in closed form.
"""
PAD = 4.0


def forbidden(pts, boxes, x_start, x_end, window):
    out = []
    for i in range(len(pts) - 1):
        sx0, sy0 = pts[i]
        sx1, sy1 = pts[i + 1]
        if sx1 <= x_start or sx0 >= x_end:
            continue
        bx, ex = max(sx0, x_start), min(sx1, x_end)
        if ex <= bx:
            continue
        k = (sy1 - sy0) / (sx1 - sx0) if sx1 - sx0 > 1e-9 else 0.0
        for bx0, bx1, by0, by1 in boxes:
            cx0, cx1 = max(bx, bx0), min(ex, bx1)
            if cx1 <= cx0:
                continue
            ya, yb = sy0 + k * (cx0 - sx0), sy0 + k * (cx1 - sx0)
            lo_y, hi_y = min(ya, yb), max(ya, yb)
            a, b = by0 - hi_y - PAD, by1 - lo_y + PAD
            if b < -window or a > window:
                continue
            out.append((a, b))
    out.sort()
    merged = []
    for a, b in out:
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    return merged


def fit_shift(pts, boxes, x_start, x_end, licence):
    """Returns the shift, or None when nothing within the licence fits."""
    window = licence + 100.0
    forb = forbidden(pts, boxes, x_start, x_end, window)
    if not forb:
        return 0.0

    def clear_at(d):
        lo, hi = -window, window
        for a, b in forb:
            if a <= d <= b:
                return None
            if b < d:
                lo = max(lo, b)
            else:
                hi = min(hi, a)
                break
        return (lo, hi) if hi > lo else None

    def corridor(band):
        lo, hi = band
        return (hi - lo) <= 60.0 and lo > -window + 0.5 and hi < window - 0.5

    def centre(band):
        c = (band[0] + band[1]) * 0.5
        return max(-licence, min(licence, c))

    band = clear_at(0.0)
    if band is not None:
        return centre(band) if corridor(band) else 0.0
    cov = next(((a - 1.0, b + 1.0) for a, b in forb if a <= 0.0 <= b), None)
    if cov is None:
        return 0.0
    best = None
    for c in cov:
        if abs(c) > licence:
            continue
        bnd = clear_at(c)
        if bnd is None:
            continue
        corr = corridor(bnd)
        if best is None or (corr and not best[1]) or (corr == best[1] and abs(c) < abs(best[0])):
            best = (c, corr, bnd)
    if best is None:
        return None
    return best[0] + (centre(best[2]) if best[1] else 0.0)


# ------------------------------------------------------------------ the scenario
# A 70-unit corridor whose floor climbs 1 unit every 40 units of x: over 8000 units it rises 200,
# far more than any single height can absorb. Blocks are 30x30, laid along both walls.
def corridor_boxes(length=8000.0, climb=1.0 / 40.0, gap=70.0, base=200.0):
    boxes = []
    x = 0.0
    while x < length:
        floor = base + climb * x
        boxes.append((x, x + 30.0, floor - 30.0, floor))          # under the corridor
        boxes.append((x, x + 30.0, floor + gap, floor + gap + 30.0))  # over it
        x += 30.0
    return sorted(boxes)


def zigzag(length=8000.0, run=30.0, y0=235.0):
    """A rigid wave: alternating +-1, never sliding, so it cannot climb with the corridor."""
    pts, hold, x, y, up = [(0.0, y0)], [], 0.0, y0, True
    while x < length:
        y += run if up else -run
        x += run
        pts.append((x, y))
        hold.append(1 if up else 0)
        up = not up
    return pts, hold


def outside(pts, boxes, step=10.0):
    """How many sampled points of the drawn line sit inside geometry."""
    n = 0
    x = pts[0][0]
    while x <= pts[-1][0]:
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
        x += step
    return n


boxes = corridor_boxes()
pts, hold = zigzag()
ok = True


def check(name, good, detail=''):
    global ok
    print('%-52s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


# 1. one height for the whole thing is impossible - the premise of the whole pass
whole = fit_shift(pts, boxes, -1e18, 8000.0, 96.0)
check('no single height fits the whole drifting corridor', whole is None,
      'global fit returned %s' % ('None' if whole is None else '%.1f' % whole))

# 2. short windows DO have a feasible band
w1200 = fit_shift(pts, boxes, 0.0, 1200.0, 96.0)
check('a 1200-unit window does have one', w1200 is not None,
      '' if w1200 is None else 'shift %.1f' % w1200)

# 3. the drift pass, exactly as the mod runs it
WIN, ADV, WLIC, RAMP = 800.0, 400.0, 45.0, 200.0
before = outside(pts, boxes)
drift = [list(p) for p in pts]
applied, cum = 0, 0.0
wx = drift[0][0]
while wx < drift[-1][0]:
    we = min(drift[-1][0], wx + WIN)
    if we - wx < 240.0:
        break
    d = fit_shift([tuple(p) for p in drift], boxes, wx, we, WLIC)
    if d is not None and abs(d) >= 0.5:
        for p in drift:
            if p[0] > wx:
                p[1] += d * min(1.0, (p[0] - wx) / RAMP)
        applied += 1
        cum += d
    wx += ADV
after = outside([tuple(p) for p in drift], boxes)
check('drift pass puts the route back in the corridor', after < before / 4,
      'buried samples %d -> %d over %d windows, cum %+.1f' % (before, after, applied, cum))

# 4. it must track the climb, not just wander: the correction should end near the true rise (200)
end_shift = drift[-1][1] - pts[-1][1]
check('the correction tracks the corridor it is following', 150.0 <= end_shift <= 250.0,
      'route rose %.1f units against a corridor that rose 200.0' % end_shift)

# 5. a corridor that does NOT drift must be left alone
flat_boxes = corridor_boxes(climb=0.0)
fpts, _ = zigzag()
fd = [list(p) for p in fpts]
wx, moved = fd[0][0], 0.0
while wx < fd[-1][0]:
    we = min(fd[-1][0], wx + WIN)
    if we - wx < 240.0:
        break
    d = fit_shift([tuple(p) for p in fd], flat_boxes, wx, we, WLIC)
    if d is not None and abs(d) >= 0.5:
        for p in fd:
            if p[0] > wx:
                p[1] += d * min(1.0, (p[0] - wx) / RAMP)
        moved += abs(d)
    wx += ADV
check('a corridor that does not drift is left alone', moved < 20.0,
      'total movement %.1f units' % moved)

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
