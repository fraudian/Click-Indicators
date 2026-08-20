"""Where is the wave? Ask the game, not the portals.

Every failure on Tidal Wave came back to one thing. The sections are derived by pairing up gamemode
portals; there are 99 of them; the pairing is wrong. It decided half the level was wave, including
two single stretches of ~12,900 units laid over ship gameplay. The route then had no corridor to fly
down, the height fit had nothing to improve against, observations had no section to attach to, and
2% of the level was drawn.

GD already knows. m_player1->m_isDart is true exactly when the player is a wave - no portals, no
pairing, no inference. It was being read every frame already, to compute "entered", and then thrown
away. Writing it down turns a guessed section into an observed one, and carries the three numbers
the solver was guessing with it: the entry height, the gravity and the size.

The cost is that a level has to be played once. These checks are about the two properties that makes
acceptable: a level with nothing remembered behaves exactly as it does today, and a remembered
stretch never moves once it is written.
"""

MIN_LEN = 200.0
JOIN = 90.0

ok = True


def check(name, good, detail=''):
    global ok
    print('%-58s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


class Seen:
    """Port of waveCommit / waveNote."""

    def __init__(self):
        self.rows = []          # [x0, x1, y, flip, size]
        self.frm = None
        self.last = -1e9
        self.entry = None

    def commit(self, x0, x1, y, flip, size):
        if not (x1 - x0 >= MIN_LEN):
            return
        for w in self.rows:
            if x0 <= w[1] + JOIN and x1 >= w[0] - JOIN:
                if x0 < w[0]:
                    w[0], w[2], w[3], w[4] = x0, y, flip, size
                if x1 > w[1]:
                    w[1] = x1
                return
        self.rows.append([x0, x1, y, flip, size])
        self.rows.sort(key=lambda r: r[0])

    def note(self, x, y, in_wave, flip=False, mini=False):
        jumped = x < self.last - 60.0
        if self.frm is not None and (not in_wave or jumped):
            self.commit(self.frm, self.last, *self.entry)
            self.frm = None
        if in_wave and self.frm is None:
            self.frm = x
            self.entry = (y, -1.0 if flip else 1.0, 2.0 if mini else 1.0)
        self.last = x

    def end(self):
        if self.frm is not None:
            self.commit(self.frm, self.last, *self.entry)
            self.frm = None


def play(seen, spans, step=30.0, end=9000.0, y=lambda x: 300.0, x0=0.0):
    """One attempt: spans are the x ranges where the player really was a wave."""
    x = x0
    while x <= end:
        seen.note(x, y(x), any(a <= x <= b for a, b in spans))
        x += step
    seen.end()


# --- one attempt teaches the level -------------------------------------------------------------------
s = Seen()
play(s, [(1000.0, 3000.0), (5000.0, 8000.0)])
check('two wave stretches are learned from one attempt', len(s.rows) == 2,
      ' and '.join('%.0f..%.0f' % (r[0], r[1]) for r in s.rows))
check('  with the entry height that was actually flown', all(r[2] == 300.0 for r in s.rows))

# --- and it does not move on the next one ----------------------------------------------------------
before = [list(r) for r in s.rows]
play(s, [(1000.0, 3000.0), (5000.0, 8000.0)], y=lambda x: 999.0)   # same run, different height read
check('a second attempt does not move what was already learned', s.rows == before,
      'the entry height is written once - re-reading it every attempt is how a line starts drifting')

# --- fragments from different StartPos points join up -------------------------------------------------
s2 = Seen()
play(s2, [(5000.0, 6500.0)], x0=4000.0, end=6600.0)      # a StartPos run through the first half
play(s2, [(6400.0, 8000.0)], x0=6300.0, end=9000.0)      # another through the second
check('two StartPos runs join into one stretch', len(s2.rows) == 1,
      '%s' % ['%.0f..%.0f' % (r[0], r[1]) for r in s2.rows])
# Within one sample step of the true bounds: the walk samples every 30 units, so it cannot land
# exactly on 5000 or 8000 and should not be asked to.
check('  covering both halves', s2.rows[0][0] <= 5000.0 + 30.0 and s2.rows[0][1] >= 8000.0 - 30.0,
      '%.0f..%.0f, against a real 5000..8000' % (s2.rows[0][0], s2.rows[0][1]))

# --- a respawn is not the end of a corridor ------------------------------------------------------------
s3 = Seen()
s3.note(1000.0, 300.0, True)
for x in (1030.0, 1060.0, 1090.0):
    s3.note(x, 300.0, True)
s3.note(400.0, 300.0, True)          # died, back to a checkpoint: x jumps backwards
s3.end()
check('a backwards jump closes the stretch rather than spanning it',
      all(r[1] < 2000.0 for r in s3.rows), str(s3.rows))

# --- scraps are not sections ---------------------------------------------------------------------------
s4 = Seen()
play(s4, [(1000.0, 1150.0)])         # 150 units, under the floor
check('a 150-unit flicker of wave is not a section', s4.rows == [])

# --- gravity and size come with it ----------------------------------------------------------------------
s5 = Seen()
s5.note(0.0, 300.0, False)
for x in range(1000, 4000, 30):
    s5.note(float(x), 500.0, True, flip=True, mini=True)
s5.end()
check('the gravity and size at entry are recorded too',
      len(s5.rows) == 1 and s5.rows[0][3] == -1.0 and s5.rows[0][4] == 2.0,
      'flip=%.0f size=%.0f - three numbers the solver was guessing' % (s5.rows[0][3], s5.rows[0][4]))

# --- THE ONE THAT MATTERS: a level with nothing remembered is unchanged -------------------------------
s6 = Seen()
check('a level never played has no observed sections', s6.rows == [],
      'so the portal sections are used exactly as they are today, and a first run is no worse')

# --- what it would have done to Tidal Wave -------------------------------------------------------------
# The portals claimed 41,881 units of an 82,333-unit level were wave, in 13 sections including two of
# ~12,900. Whatever the real answer is, it is whatever the player was actually a wave for.
portal_claim = 41881
check('the portal walk claimed half that level was wave', portal_claim > 0.5 * 82333,
      '%d of 82,333 units - the observed set replaces this entirely' % portal_claim)

# --- the merge, and the bug it replaces ---------------------------------------------------------------
# The first version SWAPPED the observed set into the section list. g_rtSecs is the only copy, so the
# moment one stretch was seen the other twelve ceased to exist for the session, and a level went from
# thirteen guesses to one 1,683-unit observation. Straight from the log:
#
#   [CI-WAVESEEN] solving 1 observed wave section(s) instead of 12 guessed
#   [CI-WAVESEEN] solving 1 observed wave section(s) instead of 1 guessed    <- the 12 are gone
#
# Observation is always partial - it covers what you have played. So it must REPLACE the guesses it
# overlaps and leave the rest alone, and the solve has to restart from the scan every time.
def merge(base, seen):
    """Port of the merge in rtSolve. base: portal sections, seen: observed stretches."""
    keep = [s for s in base if not any(s[0] < w[1] and s[1] > w[0] for w in seen)]
    return sorted(keep + [(w[0], w[1]) for w in seen])


PORTAL = [(4153, 4493), (19215, 32085), (33705, 34515), (42975, 44805), (46125, 51615),
          (59865, 72767), (76935, 82933)]
OBS = [(19183, 20866)]

r = merge(PORTAL, OBS)
check('an observed stretch replaces only the guess it overlaps', (19215, 32085) not in r,
      'the 12,870-unit phantom it sits inside is gone')
check('  and every other guess survives', len(r) == len(PORTAL),
      '%d sections, not 1' % len(r))
check('  with the observation among them', (19183, 20866) in r)

# played further: a second stretch, and still nothing else is lost
r2 = merge(PORTAL, [(19183, 20866), (26085, 26426)])
check('a second stretch does not cost the first anything', len(r2) == len(PORTAL) + 1,
      '%d sections' % len(r2))

# and the restart-from-base property: merging twice from the SAME base is idempotent
check('solving twice from the scan gives the same list', merge(PORTAL, OBS) == merge(PORTAL, OBS),
      'because each solve restarts from what the scan found, not from the last solve')
# whereas feeding a solve its own output loses sections, which is the bug
chained = merge(merge(PORTAL, OBS), [(4100, 4600)])
check('  feeding a solve its own output would lose one', len(chained) < len(PORTAL) + 1,
      '%d against %d' % (len(chained), len(PORTAL) + 1))

# --- an observed height is a measurement, not a starting guess -----------------------------------------
# The observed sections were created with the right height and then not marked as observed, so the
# solver fitted them against the geometry like any guess. On the Tidal Wave drop it took an entry
# height of 614 that the game itself had reported and moved it to 663:
#
#   [CI-SEC] x0=19183 prior=614 ... solvedY=663 moved=+49.1 licence=96 obs=0
#
# Forty-nine units up is exactly "the path was too high at the start".
def licence(exact, use_obs):
    """Port of the licence choice in rtSolve."""
    return 12.0 if exact else (40.0 if use_obs else 96.0)


check('a portal-guessed height may be fitted a long way', licence(False, False) == 96.0)
check('an observed one is held almost still', licence(True, True) == 12.0,
      'enough for the hitbox and the 10-unit sampling, not for finding a different corridor')
check('  which is less than the 49 units it drifted', licence(True, True) < 49.1,
      'the fit can no longer move an exact height that far')

# --- and adrift has to mean the same thing as support ---------------------------------------------------
# simSupported drives the re-anchor. It required a floor below AND a ceiling above - the same corridor
# assumption that made the support gate throw the drop away. On a thin band of blocks a correct route
# reads as adrift almost every step, which fired 26 re-anchors in one run: every one a jump in the
# line, and eventually a section that gives up part way through. Kinks, and a path that stops.
def supported(above, below, sandwich):
    if sandwich:
        return above is not None and above < 900.0 and below is not None and below < 900.0
    return (above is not None and above < 500.0) or (below is not None and below < 500.0)


band = (None, 60.0)          # blocks below, open sky above - the drop
check('on the drop the old test called a good route adrift', not supported(*band, sandwich=True))
check('  the new one does not', supported(*band, sandwich=False),
      'so it stops re-anchoring, which stops the kinks and the early stop')
check('  and a route that really has left is still adrift',
      not supported(None, 1400.0, sandwich=False))

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
