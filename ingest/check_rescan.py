"""When may the level be scanned again?

Three scans of the SAME level in one session, from a real Tidal Wave log:

    99 gm portals, 291 size portals, 12,616 solids, floor 475
    97 gm portals, 289 size portals,  7,691 solids, floor  91     <- a rescan mid-attempt
    99 gm portals, 291 size portals, 12,616 solids, floor 475

Thirty-nine per cent of the level missing. The cause was the cache key: the scan was redone whenever
pl->m_objects->count() CHANGED, and GD destroys objects as a level plays - so the count falls during
an attempt, that read as "a different level", and the mod re-collected whatever was left.

The route was then solved against the holes. It flew through geometry that was still there, finished
300 to 500 units from where the run actually was, and had to be teleported back into a corridor 79
times. And because different attempts cull at different points, the same section came out 18%
supported on one try and 97% on another - which is why the line moved between attempts, the one
thing this feature is not allowed to do.

A count that GROWS is a level still loading and is worth another look. A count that FALLS is a level
being played. The rule is that simple, and this is it.
"""

ok = True


def check(name, good, detail=''):
    global ok
    print('%-58s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


class Scanner:
    """Port of rtEnsure's cache decision."""

    def __init__(self):
        self.geo_ok = False
        self.geo_for = None
        self.geo_level = -1
        self.geo_count = -1
        self.scans = 0

    def ensure(self, layer, level_id, count):
        new_level = (layer is not self.geo_for) or (level_id != self.geo_level)
        if new_level:
            self.geo_count = -1
        if (not self.geo_ok) or new_level or count > self.geo_count:
            self.geo_for = layer
            self.geo_level = level_id
            if count > self.geo_count:
                self.geo_count = count
            self.scans += 1
            self.geo_ok = True
            return True          # rescanned
        return False


LAYER_A, LAYER_B = object(), object()

# --- the bug, as it happened ------------------------------------------------------------------------
s = Scanner()
s.ensure(LAYER_A, 86407629, 12616)          # load: the whole level
before = s.scans
for cnt in (12100, 11480, 9002, 7691, 7002):   # objects destroyed as the attempt runs
    s.ensure(LAYER_A, 86407629, cnt)
check('a level being played is never re-scanned', s.scans == before,
      '%d rescans over five drops from 12,616 to 7,002' % (s.scans - before))
check('  and the geometry kept is the fullest seen', s.geo_count == 12616,
      'watermark %d' % s.geo_count)

# --- but a level still LOADING must be picked up ---------------------------------------------------
s2 = Scanner()
s2.ensure(LAYER_A, 123, 400)                # scanned too early, level half built
early = s2.scans
# A list, not any(): any() short-circuits on the first True, so only the first growth step
# would actually be fed in and the watermark would stop at 2,000.
grew = [s2.ensure(LAYER_A, 123, c) for c in (2000, 9000, 12616)]
grew = any(grew)
check('a level still loading is scanned again', grew and s2.scans > early,
      '%d further scans as it filled out' % (s2.scans - early))
check('  and settles once it stops growing',
      not s2.ensure(LAYER_A, 123, 12616) and not s2.ensure(LAYER_A, 123, 12000))

# --- a different level is still a different level ----------------------------------------------------
s3 = Scanner()
s3.ensure(LAYER_A, 111, 5000)
check('another level id rescans', s3.ensure(LAYER_A, 222, 300),
      'even though its count is far lower')
check('  and its watermark starts fresh', s3.geo_count == 300,
      'or a big level would stop a small one being scanned at all')
check('another PlayLayer rescans', s3.ensure(LAYER_B, 222, 250),
      'two local levels both report id 0, and GD reuses the address')

# --- the property that matters: one scan per attempt, so the route cannot move ------------------------
s4 = Scanner()
s4.ensure(LAYER_A, 86407629, 12616)
n = s4.scans
# a full attempt: culling, a death, a respawn, more culling
for cnt in (12000, 9000, 7691, 12616, 12000, 8000, 7691):
    s4.ensure(LAYER_A, 86407629, cnt)
check('a whole attempt, deaths included, produces one geometry',
      s4.scans == n, '%d extra scans' % (s4.scans - n))
check('  which is what stops the drawn path moving between tries', s4.geo_count == 12616)

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
