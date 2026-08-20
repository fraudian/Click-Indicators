"""Within an established WAVE run, does the trajectory ever leave +-1 / +-2?

The previous pass measured every non-flat frame, which folded in ship and UFO arcs and told us
nothing. This isolates the wave properly: find long runs of frames whose |dy/dx| is exactly a wave
magnitude, then look at what interrupts them. An interruption that is short, has a non-wave
gradient, and is bracketed by wave on both sides is what "the wave slid along a surface" would
look like in recorded data.

If interruptions are essentially absent, D-blocks do not bend the wave's path and the route solver
needs no new physics - only the height fit's idea of what counts as a wall could be affected.
"""
import glob
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_traj import load  # noqa: E402

WAVE = lambda s: abs(s - 1.0) < 0.02 or abs(s - 2.0) < 0.02

files = sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'blobs', '*.gdr')))

waveFrames = 0
gaps = []           # (len, gradient) for interruptions bracketed by wave
gapGrad = Counter()
scanned = 0

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
    scanned += 1
    frames = sorted(pos)

    # per-frame slope, None where undefined
    sl = []
    for k in range(1, len(frames)):
        a, b = frames[k - 1], frames[k]
        if b != a + 1:
            sl.append(None); continue
        x0, y0 = pos[a]; x1, y1 = pos[b]
        dx = x1 - x0
        sl.append(None if dx <= 0.0001 else (y1 - y0) / dx)

    # walk: inside a wave run (>=25 wave frames seen recently), measure interruptions
    i = 0
    n = len(sl)
    while i < n:
        # find the start of a wave run
        if sl[i] is None or not WAVE(abs(sl[i])):
            i += 1; continue
        j = i
        cnt = 0
        while j < n and sl[j] is not None and WAVE(abs(sl[j])):
            cnt += 1; j += 1
        if cnt < 25:
            i = j; continue
        waveFrames += cnt
        # j is the first non-wave frame. How long until wave resumes?
        k = j
        while k < n and (sl[k] is None or not WAVE(abs(sl[k]))):
            k += 1
        if k < n and (k - j) <= 40:
            # bracketed interruption: wave -> something -> wave
            grads = [abs(sl[q]) for q in range(j, k) if sl[q] is not None]
            if grads:
                g = sum(grads) / len(grads)
                gaps.append((k - j, g))
                gapGrad[round(g, 1)] += 1
        i = k if k > i else i + 1

print("files scanned                    : %d" % scanned)
print("frames inside established waves   : %d" % waveFrames)
print("interruptions bracketed by wave   : %d" % len(gaps))
if waveFrames:
    interrupted = sum(g[0] for g in gaps)
    print("frames spent interrupted          : %d  (%.4f%% of wave frames)"
          % (interrupted, 100.0 * interrupted / waveFrames))
print()
if gaps:
    lens = sorted(g[0] for g in gaps)
    print("interruption length: median %d frames, p90 %d, max %d"
          % (lens[len(lens)//2], lens[int(len(lens)*0.9)], lens[-1]))
    print("gradients seen during interruptions:")
    for g, c in gapGrad.most_common(10):
        print("   %-5s  %d" % (g, c))
else:
    print("NO interruptions at all - the wave never leaves +-1/+-2 once established.")
