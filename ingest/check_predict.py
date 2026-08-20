"""Where will a trigger-driven platform be when the run gets there?

Levels like Thinking Space 2 build their geometry out of objects that move. The scan reads every
rect once, so a platform a trigger later raises is remembered where it started - and the route rests
on a floor that is no longer there.

The thing that makes this tractable is that the answer is STATIC. Each object only has to be placed
where it will be at the one moment the run reaches it, and that is a single fixed position, so the
route is still solved once rather than chased every frame.

This is a port of rtPredictOffset and rtEase from src/main.cpp, checked against the cases that
actually matter: a move that finished long before the run arrives, one still in progress, one that
has not fired yet, several stacked on one group, and an object belonging to several groups.
"""


def ease(p, kind, rate):
    if p <= 0.0:
        return 0.0
    if p >= 1.0:
        return 1.0
    r = rate if 0.1 < rate < 20.0 else 2.0
    if kind in (1, 4, 7, 10, 13, 16):          # InOut
        return 0.5 * (2.0 * p) ** r if p < 0.5 else 1.0 - 0.5 * (2.0 * (1.0 - p)) ** r
    if kind in (2, 5, 8, 11, 14, 17):          # In
        return p ** r
    if kind in (3, 6, 9, 12, 15, 18):          # Out
        return 1.0 - (1.0 - p) ** r
    return p                                    # None / unrecognised


def predict(arrive_t, groups, trigs):
    """(ox, oy) or None when the object's arrival time is unknown."""
    if not (arrive_t > 0.0) or not groups:
        return None
    ox = oy = 0.0
    for fire_t, group, dx, dy, dur, kind, rate in trigs:
        if group not in groups:
            continue
        p = (arrive_t - fire_t) / dur if dur > 0.001 else (1.0 if arrive_t >= fire_t else 0.0)
        p = ease(p, kind, rate)
        ox += dx * p
        oy += dy * p
    return ox, oy


ok = True


def check(name, good, detail=''):
    global ok
    print('%-56s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


# --- 1. finished long before the run arrives: the platform is simply at its destination ----------
t = [(2.0, 7, 0.0, 120.0, 1.0, 0, 2.0)]           # fires at t=2, rises 120 over 1s
r = predict(10.0, {7}, t)
check('a move that finished is at its destination', abs(r[1] - 120.0) < 1e-9,
      'offset y = %.1f, expected 120' % r[1])

# --- 2. not fired yet: the platform has not moved -------------------------------------------------
r = predict(1.0, {7}, t)
check('a move that has not fired yet has not moved', abs(r[1]) < 1e-9, 'offset y = %.1f' % r[1])

# --- 3. mid travel, linear: exactly half way -----------------------------------------------------
r = predict(2.5, {7}, t)
check('half way through a linear move is half the offset', abs(r[1] - 60.0) < 1e-9,
      'offset y = %.1f, expected 60' % r[1])

# --- 4. easing changes the middle but never the ends ----------------------------------------------
worst_end, worst_mid = 0.0, 0.0
for kind in range(0, 19):
    te = [(2.0, 7, 0.0, 120.0, 1.0, kind, 2.0)]
    worst_end = max(worst_end, abs(predict(9.0, {7}, te)[1] - 120.0),
                    abs(predict(1.0, {7}, te)[1]))
    worst_mid = max(worst_mid, abs(predict(2.5, {7}, te)[1] - 60.0))
check('every easing agrees at both ends', worst_end < 1e-9,
      'worst end error %.2e over 19 easing types' % worst_end)
check('and they genuinely differ in the middle', worst_mid > 10.0,
      'worst middle difference %.1f units' % worst_mid)

# --- 5. several triggers on one group stack -------------------------------------------------------
stacked = [(1.0, 7, 0.0, 60.0, 0.5, 0, 2.0),
           (3.0, 7, 0.0, -30.0, 0.5, 0, 2.0),
           (5.0, 7, 90.0, 0.0, 0.5, 0, 2.0)]
r = predict(9.0, {7}, stacked)
check('several moves on one group add up', abs(r[0] - 90.0) < 1e-9 and abs(r[1] - 30.0) < 1e-9,
      'offset = (%.1f, %.1f), expected (90, 30)' % r)

# a run arriving between them sees only what has happened by then
r = predict(2.0, {7}, stacked)
check('a run arriving early sees only what has fired', abs(r[1] - 60.0) < 1e-9 and abs(r[0]) < 1e-9,
      'offset = (%.1f, %.1f), expected (0, 60)' % r)

# --- 6. an object in several groups collects all of them ------------------------------------------
multi = [(1.0, 7, 0.0, 60.0, 0.5, 0, 2.0), (1.0, 12, 45.0, 0.0, 0.5, 0, 2.0)]
r = predict(9.0, {7, 12}, multi)
check('an object in two groups gets both moves', abs(r[0] - 45.0) < 1e-9 and abs(r[1] - 60.0) < 1e-9,
      'offset = (%.1f, %.1f)' % r)
r = predict(9.0, {7}, multi)
check('and not the moves aimed at groups it is not in', abs(r[0]) < 1e-9 and abs(r[1] - 60.0) < 1e-9,
      'offset = (%.1f, %.1f)' % r)

# --- 7. an instant move (zero duration) is all or nothing ------------------------------------------
inst = [(4.0, 7, 0.0, 200.0, 0.0, 0, 2.0)]
check('a zero duration move is instant', abs(predict(4.5, {7}, inst)[1] - 200.0) < 1e-9
      and abs(predict(3.5, {7}, inst)[1]) < 1e-9)

# --- 8. no arrival time, or no groups: it declines rather than guessing -----------------------------
check('declines when the arrival time is unknown', predict(-1.0, {7}, t) is None)
check('declines when the object is in no group', predict(5.0, set(), t) is None)

# --- 9. a move already applied when the geometry was read must not be applied again ---------------
# The scan reads each rect as it finds it, so anything a trigger has ALREADY done is baked into that
# rect. Measured on a real level: one trigger fires at t=0.2 and drops 358 objects by 90, the scan
# happens after it, and adding the full -90 on top put the route 90 units under the platform. Only
# the movement between the scan and the run's arrival may be applied.
def delta(scan_t, arrive_t, groups, trigs):
    a = predict(arrive_t, groups, trigs)
    b = predict(scan_t, groups, trigs)
    if a is None:
        return None
    return (a[0] - (b[0] if b else 0.0), a[1] - (b[1] if b else 0.0))


start = [(0.2, 19, 0.0, -90.0, 1.2, 0, 2.0)]        # the real one, from the log
r = delta(3.0, 120.8, {19}, start)                   # scanned well after it finished
check('a move finished before the scan is not applied again', abs(r[1]) < 1e-9,
      'offset y = %.1f, expected 0' % r[1])

r = delta(0.0, 120.8, {19}, start)                   # scanned before it fired
check('a move that fires after the scan is applied in full', abs(r[1] + 90.0) < 1e-9,
      'offset y = %.1f, expected -90' % r[1])

r = delta(0.8, 120.8, {19}, start)                   # scanned half way through it
check('a move half done at scan time contributes only its remainder',
      -90.0 < r[1] < -1.0, 'offset y = %.1f, expected between -89 and -1' % r[1])

# and a later move is unaffected by any of this
later = [(0.2, 19, 0.0, -90.0, 1.2, 0, 2.0), (100.0, 19, 0.0, 150.0, 1.0, 0, 2.0)]
r = delta(3.0, 120.8, {19}, later)
check('a move still to come is applied in full', abs(r[1] - 150.0) < 1e-9,
      'offset y = %.1f, expected 150' % r[1])

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
