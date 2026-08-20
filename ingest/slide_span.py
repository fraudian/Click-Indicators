"""How far does a real slide displace the run?

That number sets the cap on the carried offset, and guessing it is what produced both failures: 90
units let a route drift nine times the width of its own corridor, 34 truncated a slide halfway and
left the rest of it buried inside the block.

A slide's displacement is exactly its x-span times the slope it would otherwise have held, because
during it the run travels flat instead of at +-1 or +-2. So measure the x-span of every flat stretch
that sits inside an established wave run, and the slope on either side of it.
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_traj import load  # noqa: E402

WAVE = lambda s: abs(s - 1.0) < 0.02 or abs(s - 2.0) < 0.02

spans = []          # (x span, slope either side, implied displacement)
files = sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'blobs', '*.gdr')))

for f in files:
    o = load(f)
    if not isinstance(o, dict):
        continue
    fixes = o.get('frameFixes')
    if not fixes:
        continue
    pos = {}
    for fx in fixes:
        if isinstance(fx, dict) and 'frame' in fx:
            p1 = fx.get('p1') or {}
            if 'x' in p1 and 'y' in p1:
                pos[fx['frame']] = (p1['x'], p1['y'])
    if len(pos) < 500:
        continue
    frames = sorted(pos)

    sl = []
    for k in range(1, len(frames)):
        a, b = frames[k - 1], frames[k]
        if b != a + 1:
            sl.append(None); continue
        x0, y0 = pos[a]; x1, y1 = pos[b]
        dx = x1 - x0
        sl.append(None if dx <= 0.0001 else (y1 - y0) / dx)

    i, n = 0, len(sl)
    while i < n:
        if sl[i] is None or not WAVE(abs(sl[i])):
            i += 1; continue
        j, cnt = i, 0
        while j < n and sl[j] is not None and WAVE(abs(sl[j])):
            cnt += 1; j += 1
        if cnt < 25:
            i = j; continue
        before = abs(sl[j - 1])
        k = j
        while k < n and (sl[k] is None or not WAVE(abs(sl[k]))):
            k += 1
        if k < n and (k - j) <= 40:
            grads = [abs(sl[q]) for q in range(j, k) if sl[q] is not None]
            if grads and sum(grads) / len(grads) < 0.25:      # flat: a slide, not another gamemode
                xa = pos[frames[j]][0]
                xb = pos[frames[min(k, len(frames) - 1)]][0]
                span = xb - xa
                if span > 0:
                    spans.append((span, before, span * before))
        i = k if k > i else i + 1

if not spans:
    print('no flat stretches found')
    raise SystemExit(0)

disp = sorted(s[2] for s in spans)
xs = sorted(s[0] for s in spans)


def pct(v, p):
    return v[min(len(v) - 1, int(len(v) * p))]


print('flat stretches measured : %d' % len(spans))
print()
print('x span of a slide       : median %.1f  p90 %.1f  p99 %.1f  max %.1f units'
      % (pct(xs, 0.5), pct(xs, 0.9), pct(xs, 0.99), xs[-1]))
print('displacement it causes  : median %.1f  p90 %.1f  p99 %.1f  max %.1f units'
      % (pct(disp, 0.5), pct(disp, 0.9), pct(disp, 0.99), disp[-1]))
print()
for cap in (20, 34, 45, 60, 90, 120):
    covered = sum(1 for d in disp if d <= cap)
    print('  a cap of %3d units covers %5.1f%% of real slides' % (cap, 100.0 * covered / len(disp)))
