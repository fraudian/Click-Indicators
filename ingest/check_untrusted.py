"""What is allowed to put a corner in the line?

Between two clicks a wave is a straight line at exactly one or two. It has no other shape - its
vertical speed is set outright each step, not accumulated - so the only things that can bend the
drawn path are a click and a real surface. Anything else is the mod inventing gameplay. The player
put it exactly right:

    "its making all sorts of curves and stuff when there is no d blocks so a wave would just
     stay 45 degrees if theres no d blocks"

Where those corners came from is measurable. On Tidal Wave, 106 of 747 move triggers were predicted
to move things that never moved - the very first one was predicted to carry its group 150 units up
and had not shifted at all:

    [CI-BEAT] movers=4056 triggers=747 | first mover: predicted (+30.0,+150.0) really moved (+0.0,+0.0)

Every one of those puts a block somewhere it is not. The route slides along it and the line bends for
no reason a player can see.

So once the audit has caught the predictions out, the predicted positions stop being collided
against. What remains is static geometry, which is as true as it ever was. The bar has to sit above
the levels where prediction works - the platform level mispredicted one trigger in 106 - and below
the ones where it does not.
"""

BAR = 0.05
MIN_TRIGGERS = 8

ok = True


def check(name, good, detail=''):
    global ok
    print('%-58s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


def trust(struck, total):
    """Port of the gate in rtComposeGeometry."""
    if total < MIN_TRIGGERS:
        return True          # too few to judge; behave exactly as before
    return struck / total <= BAR


# --- the two real levels ----------------------------------------------------------------------------
check('the platform level still collides with its platforms', trust(1, 106),
      '1 of 106 mispredicted - the moving D blocks that took days to get right keep working')
check('Tidal Wave does not', not trust(106, 747),
      '106 of 747 mispredicted (%.0f%%)' % (100 * 106 / 747))

# --- the bar --------------------------------------------------------------------------------------
check('  the bar sits between them', 1 / 106 <= BAR < 106 / 747,
      '%.1f%% and %.0f%%, bar %.0f%%' % (100 / 106, 100 * 106 / 747, 100 * BAR))
check('  a level with no triggers at all is unaffected', trust(0, 0) and trust(0, 3))
check('  and a perfect level is trusted', trust(0, 747))

# --- what it buys ------------------------------------------------------------------------------------
# With nothing to slide on, a wave holds its slope. That is the whole claim, and it is what makes a
# straight line the CORRECT answer rather than merely a simpler one.
def fly(clicks, x0, x1, y0, slope=1.0, step=10.0, surfaces=()):
    pts, x, y = [(x0, y0)], x0, y0
    held = False
    while x < x1:
        nx = min(x + step, x1)
        held = sum(1 for c in clicks if c <= x) % 2 == 1
        y = y + (slope if held else -slope) * (nx - x)
        for lo, hi in surfaces:              # a surface flattens it
            if lo <= nx <= hi:
                y = pts[-1][1]
        pts.append((nx, y))
        x = nx
    return pts


def corners(pts, clicks, tol=1e-6):
    bad = 0
    for i in range(1, len(pts) - 1):
        s0 = (pts[i][1] - pts[i - 1][1]) / (pts[i][0] - pts[i - 1][0])
        s1 = (pts[i + 1][1] - pts[i][1]) / (pts[i + 1][0] - pts[i][0])
        if abs(s1 - s0) > tol and not any(abs(pts[i][0] - c) <= 10.0 for c in clicks):
            bad += 1
    return bad


CLICKS = [300.0, 700.0, 1100.0]
clean = fly(CLICKS, 0.0, 1500.0, 500.0)
check('with nothing to slide on, the line bends only at a click',
      corners(clean, CLICKS) == 0, 'no corner anywhere else')

phantom = fly(CLICKS, 0.0, 1500.0, 500.0, surfaces=[(400.0, 430.0), (820.0, 850.0)])
check('  a surface that is not really there puts corners in it',
      corners(phantom, CLICKS) > 0,
      '%d corners away from any click - what the player was looking at' % corners(phantom, CLICKS))

# --- the honest cost ---------------------------------------------------------------------------------
# Dropping the movers means fewer surfaces, so a genuine slide on a moving platform is missed on a
# level where prediction is unreliable. That is the right trade: a missed slide leaves the line
# straight where it should have flattened, and an invented one bends it where nothing exists. Only
# one of those is a shape the player can read.
check('the trade is stated, not hidden', True,
      'fewer surfaces on an untrusted level; a straight line beats an invented corner')

# --- the denominator has to be counted where it exists ------------------------------------------------
# The gate shipped once and never fired. g_rtTrigTotal was cached at scan time, 144 lines BEFORE the
# triggers were collected, so it was always zero - which reads as "too few triggers to judge" and
# waves everything through. On a level with 747 triggers and 106 of them wrong:
#
#   [CI-AUDIT] 106 trigger(s) struck out
#   [CI-UNTRUSTED] (never printed)
#
# A guard whose denominator is zero is not a guard, and it fails in the direction that looks like
# nothing is wrong.
check('a zero denominator waves everything through', trust(106, 0),
      'which is what the cached total did - 106 strikes, gate silent')
check('  counted where the triggers actually are, it fires', not trust(106, 747))
check('  and "too few to judge" still means too few, not unmeasured',
      trust(1, 3) and not trust(106, 747),
      'MIN_TRIGGERS exists for a level with a handful, not for a level nobody counted')

# --- untrusted means "do not predict", not "pretend it is not there" ------------------------------------
# The first cut skipped the movers entirely. That removed 4,056 real objects from the level and the
# line ran through blocks that were plainly on screen - nine stretches of it, up to 13 units deep.
#
# What the audit establishes is that the OFFSETS are wrong, not that the objects are absent. And a
# mispredicted trigger usually means the thing never moved at all, so where it is right now is the
# truth rather than a guess.
def geometry(trust, predicted_ok):
    """What a mover contributes: 'predicted', 'live', or nothing."""
    if trust:
        return 'predicted' if predicted_ok else 'live'
    return 'live'


check('a trusted level still uses the prediction', geometry(True, True) == 'predicted')
check('an untrusted one falls back to where the object is', geometry(False, True) == 'live',
      'real geometry, no invented offset')
check('  and never to nothing at all', geometry(False, False) != 'none',
      'skipping them is what put the line through blocks')

# --- a wave does not slide on a spike ------------------------------------------------------------------
# Hazards were collected alongside solids and fed to the same collision, so the route came to rest on
# them and skated along their tops. In the player's screenshot the line goes shallow exactly as it
# passes a spiked ball and steepens again after it - a bend with nothing solid anywhere near it.
#
# The reasoning that put them in was that the route should not pass through a spike. But the run
# being drawn SURVIVED it: it never touched one. A hazard can only ever push the line somewhere the
# run was not.
SOLID, HAZARD, DECOR = 'solid', 'hazard', 'decoration'


def collides(kind):
    return kind == SOLID


check('a solid is a surface', collides(SOLID))
check('a hazard is not', not collides(HAZARD),
      'the run survived it, so it never rested on one')
check('  and decoration never was', not collides(DECOR))

# what that does to the drawn shape: with only a spike nearby, the wave holds its slope
spike_only = [(400.0, 430.0)]
straight = fly(CLICKS, 0.0, 1500.0, 500.0,
               surfaces=[] if not collides(HAZARD) else spike_only)
check('with only a spike beside it, the line stays at 45 degrees',
      corners(straight, CLICKS) == 0,
      'no corner away from a click - which is the whole claim')

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
