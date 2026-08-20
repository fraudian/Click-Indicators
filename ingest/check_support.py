"""When should the mod admit it does not know?

From a real Tidal Wave log. The level is 4,056 moving objects driven by 747 triggers, its macro
carries no recorded positions at all, and 106 of those triggers were predicted to move things that
never moved - so the route is solved against a level that does not exist:

    section 19215..32085   inside the level for 40% of it
    section 42975..44805   inside the level for 10% of it
    section 45495..45727   inside the level for  0% of it
    79 re-anchors, the route landing at y=1039 / 1086 / 990 while the corridor sat at 547 / 597 / 660

The player's verdict was "not one path is readable". They were right, and no amount of drawing it
more carefully helps - the line is not near the level.

Worse, it was not the same line twice. One section came out 18% supported on one attempt and 97% on
another, because which wrong predictions get struck out differs per run. That breaks the one rule
this feature has: the path must not move.

The measurement of how much of the route sat inside the level already existed as a diagnostic. This
is it being used to decide, and the checks below are about where the bar goes: high enough to drop
the sections that made the mod look broken, low enough to keep the ones that work.
"""

BAR = 0.75
MIN_STEPS = 20

ok = True


def check(name, good, detail=''):
    global ok
    print('%-60s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


def drawn(support, steps, from_record):
    """Port of the gate in rtSolve."""
    if from_record:
        return True          # that IS where the run was; "outside the level" means the scan is wrong
    if steps <= MIN_STEPS:
        return True          # too short to judge
    return support >= BAR


# --- the sections that made the mod look broken ---------------------------------------------------
tidal = [('19215..32085', 0.40, 1373), ('19215..32085', 0.45, 1373), ('19215..32085', 0.67, 1380),
         ('42975..44805', 0.10, 202), ('42975..44805', 0.45, 202),
         ('45495..45727', 0.00, 23), ('4153..4493', 0.18, 33), ('4153..4493', 0.23, 35)]
kept = [t for t in tidal if drawn(t[1], t[2], False)]
check('every unreadable Tidal Wave section is dropped', not kept,
      'kept %s' % ([t[0] for t in kept] or 'nothing'))

# --- and the ones that are fine are untouched ------------------------------------------------------
good = [('33705..34515', 0.85, 89), ('33705..34515', 0.86, 91), ('4153..4493', 0.97, 33),
        ('0..96955', 1.00, 10303)]     # the level that tracks the run to +-20 units
check('a section that solved properly is still drawn',
      all(drawn(g[1], g[2], False) for g in good),
      '85%, 86%, 97%, 100% all kept')

# --- a recorded route is never dropped --------------------------------------------------------------
# It is where the run actually was. If that reads as "outside the level", the geometry scan is what
# is wrong, and throwing away the truth to protect a guess would be exactly backwards.
check('a route drawn from a recording is never dropped',
      drawn(0.10, 5000, True) and drawn(0.0, 5000, True),
      'even at 0% support')

# --- short sections are not judged on too little evidence -------------------------------------------
check('a section too short to measure is left alone',
      drawn(0.0, 5, False) and drawn(0.3, 20, False),
      'a handful of steps says nothing either way')
check('  but one long enough to measure is judged', not drawn(0.3, 21, False))

# --- the bar itself ---------------------------------------------------------------------------------
# The two populations in the real logs are far apart: the sections that work sit at 85-100%, the ones
# that made the mod unusable sit at 0-67%. Anywhere in between is defensible; what matters is that
# the bar separates them cleanly rather than splitting either group.
working = [0.85, 0.86, 0.97, 1.00]
broken = [0.00, 0.10, 0.18, 0.23, 0.40, 0.45, 0.67]
check('the bar separates the two populations cleanly',
      min(working) >= BAR > max(broken),
      'working %.0f%%-%.0f%%, broken %.0f%%-%.0f%%, bar %.0f%%'
      % (100 * min(working), 100 * max(working), 100 * min(broken), 100 * max(broken), 100 * BAR))
check('  with room on both sides', min(working) - BAR >= 0.05 and BAR - max(broken) >= 0.05,
      '%.0f points of margin below, %.0f above' % (100 * (BAR - max(broken)),
                                                   100 * (min(working) - BAR)))

# --- what the player ends up with --------------------------------------------------------------------
# Dropping a section is not fixing it. It is the difference between a mod that is wrong and a mod that
# is quiet, and only one of those is worth shipping.
before = len(tidal)
after = len(kept)
check('on that level the mod now draws nothing rather than nonsense',
      after == 0 and before > 0,
      '%d unreadable sections, %d drawn' % (before, after))

# --- and what "inside the level" has to mean ---------------------------------------------------------
# The first version counted steps where the route was SANDWICHED - a floor within 900 below AND a
# ceiling within 900 above. That is a corridor assumption, and the player's own screenshot of the
# Tidal Wave drop shows why it is wrong there: the drop is a thin horizontal band of blocks in open
# space, which the wave weaves through and around. There is nothing above it for hundreds of units.
# A perfectly placed route scores near zero, and the gate threw the entire drop away.
#
# What distinguishes a placed route from one that has left the level is whether there is anything
# NEAR it - either side, not both.
def sandwiched(above, below, limit=900.0):
    return above is not None and above < limit and below is not None and below < limit


def nearby(above, below, limit=400.0):
    return (above is not None and above < limit) or (below is not None and below < limit)


# a wave hugging a band of blocks with open sky above - the Tidal Wave drop
band = (None, 40.0)
check('a route on a band of blocks is not "sandwiched"', not sandwiched(*band),
      'nothing above it, so the old measure called it lost')
check('  but it IS near the level', nearby(*band),
      'blocks 40 units below - which is where the wave actually is')

# a route that has genuinely left the level
gone = (None, 1400.0)
check('a route 1,400 units above everything is neither', not sandwiched(*gone) and not nearby(*gone),
      'which is the case the gate exists for, and it still catches it')

# and a corridor still reads as placed under both
tube = (45.0, 45.0)
check('a real corridor satisfies both measures', sandwiched(*tube) and nearby(*tube))

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
