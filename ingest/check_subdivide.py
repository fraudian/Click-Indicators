"""Where is the wave, when the portals cannot be trusted?

From a real Tidal Wave log. The gamemode-portal walk produced 13 sections covering 41,881 of the
level's 82,333 units - it decided that 51% of Tidal Wave is wave gameplay, including two single
stretches of 12,870 and 12,902 units. That is about forty seconds of continuous wave, each.

It is not. Probing the drawn route inside them found one to three blocks at each position with
nothing at all above, where the level that works probes at thirty with walls both sides. Those
sections are laid over ship and cube gameplay: the route had nothing to fly down, the height fit had
nothing to improve against, and 2% of the level survived to be drawn.

A wave section IS a corridor - it has to be, because the thing moves at 45 or 63 degrees and never
stops, so walls above and below are the only way a level controls one. That makes "where is the
wave" a question about geometry, and geometry is the more reliable witness when there are 99 portals
to pair up.

The portal sections are kept and only cut up. The property that matters most is the last one here:
a level whose portals were already right must come out unchanged.
"""
import math

STEP, GAP, MIN = 30.0, 150.0, 300.0

ok = True


def check(name, good, detail=''):
    global ok
    print('%-58s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


def subdivide(x0, x1, prior, band_at):
    """Port of rtSubdivideByCorridor. Returns [(x0, x1, entry, inferred)] or [] for none."""
    if not (x1 - x0 > MIN):
        return [(x0, x1, prior, False)]
    runs, ref = [], prior
    run_from, run_entry, last_good = None, 0.0, None
    x = x0
    while x <= x1:
        c = band_at(x, ref)
        if c is not None:
            ref = c
            if run_from is None:
                run_from, run_entry = x, c
            last_good = x
        elif run_from is not None and x - last_good > GAP:
            if last_good - run_from >= MIN:
                runs.append((run_from, last_good, run_entry))
            run_from = None
        x += STEP
    if run_from is not None and last_good - run_from >= MIN:
        runs.append((run_from, last_good, run_entry))

    if len(runs) == 1 and runs[0][0] <= x0 + STEP and runs[0][1] >= x1 - STEP:
        return [(x0, x1, prior, False)]          # one corridor end to end: untouched
    return [(a, b, (prior if a <= x0 + STEP else e), a > x0 + STEP) for a, b, e in runs]


def corridor_everywhere(mid=300.0):
    return lambda x, ref: mid


def open_air():
    return lambda x, ref: None


def corridor_between(lo, hi, mid=300.0):
    return lambda x, ref: (mid if lo <= x <= hi else None)


# --- THE ONE THAT MUST NOT CHANGE ------------------------------------------------------------------
# A level whose portals were already right is one corridor from end to end. It has to come out
# byte-identical, or this fix costs more than it buys.
r = subdivide(0.0, 96955.0, 105.0, corridor_everywhere())
check('a section that is corridor end to end is untouched',
      r == [(0.0, 96955.0, 105.0, False)], str(r))
r = subdivide(4153.0, 4493.0, 419.0, corridor_everywhere())
check('  and so is one too short to subdivide', len(r) == 1 and r[0][0] == 4153.0)

# --- Tidal Wave's phantoms --------------------------------------------------------------------------
r = subdivide(19215.0, 32085.0, 615.0, open_air())
check('a 12,870-unit section over open level yields nothing', r == [],
      'the portals called it wave; there is no corridor in it')

# --- and the case the whole thing is for: real wave buried inside a phantom --------------------------
# A genuine wave stretch in the middle of a section the portals got wrong. Rejecting the section
# would lose it; subdividing keeps it.
r = subdivide(19215.0, 32085.0, 615.0, corridor_between(24000.0, 27000.0, 700.0))
check('a real wave stretch inside a phantom is kept', len(r) == 1, str(len(r)) + ' piece(s)')
if len(r) == 1:
    a, b, entry, inferred = r[0]
    check('  cut to the corridor, not the portals', a >= 24000.0 - STEP and b <= 27000.0 + STEP,
          'x %.0f..%.0f out of a 19215..32085 section' % (a, b))
    check('  its height comes from the corridor it sits in', entry == 700.0,
          'not the portal mouth at 615')
    check('  and it is marked inferred, because no portal vouched for it', inferred)

# --- several corridors in one section ---------------------------------------------------------------
def two_rooms(x, ref):
    if 1000.0 <= x <= 2000.0 or 5000.0 <= x <= 7000.0:
        return 400.0
    return None


r = subdivide(0.0, 9000.0, 400.0, two_rooms)
check('two corridors in one section become two sections', len(r) == 2,
      ' and '.join('%.0f..%.0f' % (a, b) for a, b, _, _ in r))

# --- a doorway is not the end of a corridor ---------------------------------------------------------
def with_doorway(x, ref):
    return None if 3000.0 < x < 3090.0 else 400.0      # a 90-unit gap, under GAP


r = subdivide(0.0, 6000.0, 400.0, with_doorway)
check('a short gap does not split a corridor', len(r) == 1,
      '%d piece(s) - a 90-unit doorway is not the end of anything' % len(r))


def with_chasm(x, ref):
    return None if 3000.0 < x < 3400.0 else 400.0      # 400 units, well over GAP


r = subdivide(0.0, 6000.0, 400.0, with_chasm)
check('  but a real break does', len(r) == 2, '%d piece(s)' % len(r))

# --- scraps are not worth a line ---------------------------------------------------------------------
def sliver(x, ref):
    return 400.0 if 1000.0 <= x <= 1200.0 else None    # 200 units, under MIN


check('a 200-unit scrap of corridor is not drawn', subdivide(0.0, 6000.0, 400.0, sliver) == [])

# --- what it does to that level ------------------------------------------------------------------------
tidal = [(4153, 4493), (4905, 5295), (19215, 32085), (33705, 34515), (42975, 44805),
         (45495, 45727), (45825, 46009), (46125, 51615), (56505, 56895), (59382, 59561),
         (59865, 72767), (74175, 74441), (76935, 82933)]
claimed = sum(b - a for a, b in tidal)
check('the portals claimed half the level was wave', claimed > 0.5 * 82333,
      '%d of 82,333 units (%.0f%%)' % (claimed, 100.0 * claimed / 82333))

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
