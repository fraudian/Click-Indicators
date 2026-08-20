"""When the recording covers most of the level but not all of it, is any of it used?

Straight from a real log:

    [CI-PATH] macro carries 64980 positions, x 46..96405, 0 backward steps (1 recording)
    [CI-SEC]  x0=0 x1=96955 ... route 47..96955

Sixty-five thousand real positions, in the other player's own hand, covering 99.4% of the level -
and the route was simulated end to end anyway, because the old rule wanted the recording to reach
within 400 units of BOTH ends or it took none of it. It stopped 150 units short. Everything the
user then watched go wrong past the mini portal at x=9675 was invented by a simulation that had the
answer in front of it.

Coverage is a range, not a yes or no. This checks the splice that replaced that rule: recorded
where recorded, simulated in the gaps, joined at the seams, and never worse than either alone.
"""

STEP = 10.0


def interp(poly, qx):
    if len(poly) < 2:
        return None
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


def splice(rec, sim, x0, x1):
    """Port of the splice in src/main.cpp. Returns (pts, hold, mode)."""
    if len(rec) < 8 or rec[-1][0] - rec[0][0] < 200.0:
        return list(sim), None, 'simulated'
    rx0, rx1 = rec[0][0], rec[-1][0]
    if rx0 <= x0 + 400.0 and rx1 >= x1 - 400.0:
        pts, hold = [], []
        for q in rec:
            if pts:
                hold.append(1 if q[1] > pts[-1][1] else 0)
            pts.append(q)
        return pts, hold, 'recorded'

    sh, st = interp(sim, rx0), interp(sim, rx1)
    dh = rec[0][1] - sh if sh is not None else 0.0
    dt = rec[-1][1] - st if st is not None else 0.0
    pts, hold = [], []

    def push(px, py):
        if pts:
            if px <= pts[-1][0]:
                return
            hold.append(1 if py > pts[-1][1] else 0)
        pts.append((px, py))

    for q in sim:
        if q[0] >= rx0:
            break
        push(q[0], q[1] + dh)
    for q in rec:
        push(q[0], q[1])
    for q in sim:
        if q[0] <= rx1:
            continue
        push(q[0], q[1] + dt)
    return pts, hold, 'spliced'


def wave(x_from, x_to, y0, seed, step=4.0, turn=90.0):
    """A wave path: alternating +-1, turning every `turn` units. Deterministic per seed."""
    pts, x, y, up = [], x_from, y0, seed % 2 == 1
    nxt = x_from + turn
    while x <= x_to:
        pts.append((x, y))
        x += step
        if x >= nxt:
            up = not up
            nxt += turn * (1.0 + 0.1 * ((seed * 7 + int(x)) % 5))
        y += (step if up else -step)
    return pts


ok = True


def check(name, good, detail=''):
    global ok
    print('%-58s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


X0, X1 = 0.0, 96955.0

# The real case: the recording covers 46..96405 of a section running 0..96955.
rec = wave(46.0, 96405.0, 105.0, seed=1)
sim = wave(0.0, 96955.0, 160.0, seed=2)          # a plausible-but-wrong simulation

pts, hold, mode = splice(rec, sim, X0, X1)
check('the real case is no longer thrown away', mode == 'spliced', 'mode = %s' % mode)

# --- 1. the covered stretch must be the recording EXACTLY ----------------------------------------
worst, at = 0.0, 0.0
x = rec[0][0]
while x <= rec[-1][0]:
    d = abs(interp(pts, x) - interp(rec, x))
    if d > worst:
        worst, at = d, x
    x += 137.0
check('inside the recording the line IS the recording', worst < 1e-6,
      'worst %.2e units (at x=%.0f)' % (worst, at))

# --- 2. pts/hold paired, x strictly ascending ----------------------------------------------------
check('pts and hold stay paired', len(hold) == len(pts) - 1,
      '%d holds for %d pts' % (len(hold), len(pts)))
check('x is strictly ascending', all(pts[i + 1][0] > pts[i][0] for i in range(len(pts) - 1)))
check('hold flags are 0 or 1', all(f in (0, 1) for f in hold))

# --- 3. no step at the seams. A visible jump would be worse than either half alone ---------------
def seam_jump(px, poly):
    lo = max(p for p in poly if p[0] < px)
    hi = min((p for p in poly if p[0] > px), key=lambda p: p[0])
    dx = hi[0] - lo[0]
    return abs(hi[1] - lo[1]) / dx if dx > 1e-9 else 1e18


check('the head seam is continuous', seam_jump(rec[0][0], pts) <= 2.0 + 1e-6,
      'gradient at the join %.2f (a wave is 1 or 2)' % seam_jump(rec[0][0], pts))
check('the tail seam is continuous', seam_jump(rec[-1][0], pts) <= 2.0 + 1e-6,
      'gradient at the join %.2f' % seam_jump(rec[-1][0], pts))

# --- 4. it is a strict improvement over simulating the whole thing --------------------------------
def err_vs_truth(poly, truth):
    e, n, x = 0.0, 0, truth[0][0]
    while x <= truth[-1][0]:
        e += abs(interp(poly, x) - interp(truth, x))
        n += 1
        x += 97.0
    return e / max(n, 1)


check('mean error against the run collapses', err_vs_truth(pts, rec) < 1e-6,
      'spliced %.3f vs simulated %.1f units' % (err_vs_truth(pts, rec), err_vs_truth(sim, rec)))
check('and the old rule really was throwing that away', err_vs_truth(sim, rec) > 20.0,
      'the discarded simulation was off by %.0f units on average' % err_vs_truth(sim, rec))

# --- 5. full coverage still takes the recording whole, as before ----------------------------------
full = wave(10.0, 96900.0, 105.0, seed=1)
_, _, m2 = splice(full, sim, X0, X1)
check('full coverage is unchanged (recording taken whole)', m2 == 'recorded', 'mode = %s' % m2)

# --- 6. a recording covering only the MIDDLE is still worth using ---------------------------------
mid = wave(30000.0, 60000.0, 300.0, seed=3)
pm, hm, m3 = splice(mid, sim, X0, X1)
covered = all(abs(interp(pm, x) - interp(mid, x)) < 1e-6 for x in range(30100, 59900, 991))
check('a recording covering only the middle is spliced in', m3 == 'spliced' and covered,
      'mode = %s, middle matches = %s' % (m3, covered))
check('  and it still comes out one ascending line',
      len(hm) == len(pm) - 1 and all(pm[i + 1][0] > pm[i][0] for i in range(len(pm) - 1)))

# --- 7. too little to be worth it -> leave the simulation alone -----------------------------------
tiny = wave(5000.0, 5100.0, 200.0, seed=4)
_, _, m4 = splice(tiny, sim, X0, X1)
check('a 100-unit scrap is not spliced in', m4 == 'simulated', 'mode = %s' % m4)
short = [(x * 1.0, 200.0) for x in range(4)]
_, _, m5 = splice(short, sim, X0, X1)
check('too few points is not spliced in', m5 == 'simulated', 'mode = %s' % m5)

# --- 8. the head and tail keep the simulation's SHAPE, only its height moves ----------------------
# What the simulation offers past the recording is the sequence of turns. Shifting it must not
# change a single gradient, or the cue times move with it.
def grads(poly, lo, hi):
    return [round((poly[i + 1][1] - poly[i][1]) / (poly[i + 1][0] - poly[i][0]), 6)
            for i in range(len(poly) - 1) if lo <= poly[i][0] and poly[i + 1][0] <= hi]


tail_sim = grads(sim, rec[-1][0] + 20.0, X1)
tail_out = grads(pts, rec[-1][0] + 20.0, X1)
check('the simulated tail keeps every gradient it had', tail_sim == tail_out and len(tail_sim) > 0,
      '%d gradients compared' % len(tail_sim))

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
