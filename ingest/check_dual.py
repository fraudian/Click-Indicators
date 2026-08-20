"""Is the second icon's line the right line, and does it know when it is not?

A dual portal puts a second player on the level. In wave both are flown by the SAME button with
opposite gravity, so the second is the first reflected about the height they entered the dual at -
which is why a dual wave looks like an X rather than two parallel lines.

Reflecting is a stronger claim than it looks, and it is only true of a SYMMETRIC dual. There the
geometry is mirrored too, so the reflection is the collided path exactly. Where the two halves are
built differently the second player meets different walls and slides at different places, and
nothing about it is recoverable from the first - so the mirror has to be refused rather than drawn
confidently through a wall.

This checks the reflection, the button semantics (a held segment goes UP on one line and DOWN on
the other - it is one click, not two), and that the buried test actually separates the two cases.
"""

BURIED_LIMIT = 15.0


def mirror(pts, hold, axis):
    """Port of the mirror in src/main.cpp: reflect y, carry the button flag unchanged."""
    return [(x, 2.0 * axis - y) for (x, y) in pts], list(hold)


def buried_pct(pts, hold, boxes):
    inside, seen = 0, 0
    for i in range(len(pts) - 1):
        if i < len(hold) and hold[i] == 2:
            continue
        seen += 1
        x, y = pts[i]
        for x0, x1, y0, y1 in boxes:
            if x0 > x:
                break
            if x1 < x:
                continue
            if y0 + 2.0 < y < y1 - 2.0:
                inside += 1
                break
    return 100.0 * inside / seen if seen else 100.0


def wave_in(corridor_mid, x_from, x_to, y0, step=5.0, turn=60.0):
    """A wave flown down a corridor, turning on a fixed cadence."""
    pts, hold, x, y, up = [], [], x_from, y0, True
    nxt = x_from + turn
    while x <= x_to:
        if pts:
            hold.append(1 if up else 0)
        pts.append((x, y))
        lo, hi = corridor_mid(x) - 45.0, corridor_mid(x) + 45.0
        x += step
        y += step if up else -step
        y = max(lo, min(hi, y))
        if x >= nxt:
            up = not up
            nxt += turn
    return pts, hold


def corridor_boxes(mid_fn, x_from, x_to, gap=90.0, seg=30.0):
    """Walls above and below a centre line - the shape a wave section always is."""
    boxes, x = [], x_from
    while x < x_to:
        m = mid_fn(x)
        boxes.append((x, x + seg, m - gap / 2 - 30.0, m - gap / 2))
        boxes.append((x, x + seg, m + gap / 2, m + gap / 2 + 30.0))
        x += seg
    return sorted(boxes)


ok = True


def check(name, good, detail=''):
    global ok
    print('%-58s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


AXIS = 300.0

# --- 1. the reflection itself --------------------------------------------------------------------
p1 = [(0.0, 300.0), (30.0, 330.0), (60.0, 300.0), (90.0, 240.0), (120.0, 300.0)]
h1 = [1, 0, 0, 1]
p2, h2 = mirror(p1, h1, AXIS)
check('the mirror reflects about the entry height',
      p2 == [(0.0, 300.0), (30.0, 270.0), (60.0, 300.0), (90.0, 360.0), (120.0, 300.0)],
      str(p2))

# --- 2. it is ONE click, so the button flags are identical on both lines --------------------------
check('the button flags are carried unchanged', h2 == h1, '%s vs %s' % (h2, h1))

# a held segment therefore rises on one line and falls on the other - that IS a dual wave
rises = [p1[i + 1][1] > p1[i][1] for i in range(len(p1) - 1) if h1[i] == 1]
falls = [p2[i + 1][1] < p2[i][1] for i in range(len(p2) - 1) if h2[i] == 1]
check('a held segment rises on one line and falls on the other',
      all(rises) and all(falls) and len(rises) > 0)

# --- 3. the two lines meet exactly where the run is level with the axis ---------------------------
touch = [i for i in range(len(p1)) if abs(p1[i][1] - p2[i][1]) < 1e-9]
check('the pair crosses exactly at the axis',
      all(abs(p1[i][1] - AXIS) < 1e-9 for i in touch) and len(touch) >= 2,
      '%d crossings, all at y=%.0f' % (len(touch), AXIS))

# --- 4. a SYMMETRIC dual: the mirror is a legal path and must be drawn ----------------------------
# Two corridors placed symmetrically about the axis, which is what a symmetric dual is.
def upper(x):
    return AXIS + 150.0


def lower(x):
    return AXIS - 150.0


sym = corridor_boxes(upper, 0.0, 6000.0) + corridor_boxes(lower, 0.0, 6000.0)
sym.sort()
sp, sh = wave_in(upper, 0.0, 6000.0, AXIS + 150.0)
mp, mh = mirror(sp, sh, AXIS)
b_self, b_mirror = buried_pct(sp, sh, sym), buried_pct(mp, mh, sym)
check('on a symmetric dual the mirror is a legal path',
      b_mirror <= BURIED_LIMIT,
      'mirror %.0f%% buried (the first line is %.0f%%)' % (b_mirror, b_self))
check('  and it lands in the OTHER corridor, not its own',
      all(lower(0) - 60.0 < y < lower(0) + 60.0 for (_, y) in mp[5:]),
      'mirror runs y %.0f..%.0f, other corridor is at %.0f'
      % (min(y for _, y in mp), max(y for _, y in mp), lower(0)))

# --- 5. an ASYMMETRIC dual: the mirror is through a wall and must be refused ----------------------
# Same upper corridor, but the lower half of the level is somewhere else entirely and solid where
# the mirror would go.
asym = corridor_boxes(upper, 0.0, 6000.0)
asym += [(x * 1.0, x + 30.0, AXIS - 260.0, AXIS - 60.0) for x in range(0, 6000, 30)]
asym.sort()
b_asym = buried_pct(mp, mh, asym)
check('on an asymmetric dual the mirror is refused', b_asym > BURIED_LIMIT,
      '%.0f%% of it inside solid geometry' % b_asym)
check('  and the first line is still fine there', buried_pct(sp, sh, asym) <= BURIED_LIMIT,
      '%.0f%% buried' % buried_pct(sp, sh, asym))

# --- 6. the gap marker survives the mirror -------------------------------------------------------
gp = [(0.0, 300.0), (30.0, 330.0), (60.0, 300.0), (900.0, 300.0), (930.0, 330.0)]
gh = [1, 0, 2, 1]
gm, gmh = mirror(gp, gh, AXIS)
check('a gap between sections stays a gap in the mirror', gmh[2] == 2 and gmh == gh)
check('  and a gap segment is never counted as buried',
      buried_pct(gm, gmh, [(60.0, 900.0, 200.0, 400.0)]) < 100.0)

# --- 7. reflecting twice is the identity - the mirror of the mirror is the run --------------------
back, _ = mirror(mp, mh, AXIS)
check('mirroring twice returns the original line',
      max(abs(a[1] - b[1]) for a, b in zip(back, sp)) < 1e-9)

# --- 8. the axis is the ONLY thing the mirror needs, and it is the entry height -------------------
# Off-by-one on the axis moves the whole second line by twice as much, so this is worth stating.
off = mirror(p1, h1, AXIS + 10.0)[0]
check('an axis 10 units out moves the mirror by 20',
      abs((off[1][1] - p2[1][1]) - 20.0) < 1e-9,
      'moved %.0f units' % (off[1][1] - p2[1][1]))

# --- 9. under the floor is a WRONG AXIS, not an asymmetric dual -----------------------------------
# The distinction matters: an asymmetric dual is a fact about the level and nothing can be drawn,
# where a mirror below the ground means the axis was picked wrong and the other candidate may well
# be right. Scored separately so the log can say which happened.
FLOOR = 91.0


def under_pct(pts, hold, floor=FLOOR):
    below, seen = 0, 0
    for i in range(len(pts) - 1):
        if i < len(hold) and hold[i] == 2:
            continue
        seen += 1
        if pts[i][1] < floor + 4.0:
            below += 1
    return 100.0 * below / seen if seen else 100.0


# a run low in the level, mirrored about its own entry height, ends up underground
low, lowh = wave_in(lambda x: 130.0, 0.0, 3000.0, 130.0)
under_axis = mirror(low, lowh, 125.0)[0]
check('a mirror about too low an axis lands under the floor',
      under_pct(under_axis, lowh) > 15.0,
      '%.0f%% of it below y=%.0f' % (under_pct(under_axis, lowh), FLOOR))

# the same run mirrored about a portal sitting higher up stays in the level
ok_axis = mirror(low, lowh, 300.0)[0]
check('  and about a sensible one it does not', under_pct(ok_axis, lowh) < 1.0,
      '%.0f%% below the floor' % under_pct(ok_axis, lowh))

# --- 10. the second axis is only reached for when the first one fails -----------------------------
def pick(entry_buried, portal_buried):
    """Port of the choice in src/main.cpp: the entry axis unless it is refused AND the portal is
    better. A legal entry axis is never overridden - the fallback exists to rescue a refusal, not
    to shop for the prettiest line."""
    return 'portal' if (entry_buried > BURIED_LIMIT and portal_buried < entry_buried) else 'entry'


check('a legal entry axis is never overridden', pick(3.0, 0.0) == 'entry')
check('a refused entry axis falls back to the portal', pick(70.0, 2.0) == 'portal')
check('  but not to something equally bad', pick(70.0, 70.0) == 'entry')
check('  and both bad still means refused',
      pick(70.0, 90.0) == 'entry' and 70.0 > BURIED_LIMIT)

# --- 11. the axis is the PORTAL, and that is a measurement, not a preference ----------------------
# From a real log, two dual regions, fourteen samples taken wherever the run happened to be:
#
#   x=22384 p1=192 p2=288   x=23104 p1=179 p2=301   x=35842 p1=100 p2=380
#   x=22624 p1=240 p2=240   x=23345 p1=238 p2=242   x=36563 p1=100 p2=380
#   x=22864 p1=248 p2=232   x=35121 p1=168 p2=312   x=36803 p1=136 p2=344
#
# Every one of those pairs has midpoint 240.0 exactly, and the dual portal in both regions sits at
# y=240. The entry height read 233 and 216 - and an axis 24 units out moves the second line 48.
REAL = [(192, 288), (240, 240), (248, 232), (179, 301), (238, 242), (201, 279),
        (168, 312), (202, 278), (181, 299), (100, 380), (192, 288), (181, 299),
        (100, 380), (136, 344)]
PORTAL_Y = 240.0
mids = [(a + b) / 2.0 for a, b in REAL]
check('the game mirrors about one fixed height, whatever the run does',
      max(mids) - min(mids) < 1e-9,
      'midpoint is %.1f at all %d samples (p1 ranged %d..%d)'
      % (mids[0], len(mids), min(a for a, _ in REAL), max(a for a, _ in REAL)))
check('  and that height is the dual portal', abs(mids[0] - PORTAL_Y) < 1e-9,
      'midpoint %.1f, portal y %.1f' % (mids[0], PORTAL_Y))

# the entry heights the old model produced, and what they cost
for entry, expect in ((233.0, 14.0), (216.0, 48.0)):
    err = abs(2.0 * (entry - PORTAL_Y))
    check('  an entry axis of %.0f moves the mirror %.0f units' % (entry, expect),
          abs(err - expect) < 1e-9, 'off by %.0f' % err)


def pick_axis(portal_buried, entry_buried, same):
    """Port of the choice in src/main.cpp, in its corrected order: the portal axis unless it is
    refused AND the entry axis is better. The measured model leads; the other one is a rescue."""
    if portal_buried <= BURIED_LIMIT or same:
        return 'portal'
    return 'entry' if entry_buried < portal_buried else 'portal'


check('a legal portal axis is never overridden', pick_axis(2.0, 0.0, False) == 'portal')
check('a refused portal axis can fall back to the entry height',
      pick_axis(70.0, 3.0, False) == 'entry')
check('  and does not when both are hopeless', pick_axis(70.0, 80.0, False) == 'portal')

# --- 12. stacked dual portals: the axis is their midpoint ----------------------------------------
# A level that puts a dual portal in each corridor is telling you where the two icons enter, and
# the midpoint of those is the axis. With one portal it reduces to that portal's own height.
def axis_of(portal_ys):
    return sum(portal_ys) / len(portal_ys)


check('one portal means its own height', abs(axis_of([240.0]) - 240.0) < 1e-9)
check('two stacked portals mean their midpoint', abs(axis_of([150.0, 330.0]) - 240.0) < 1e-9,
      'portals at 150 and 330 -> axis %.0f' % axis_of([150.0, 330.0]))

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
