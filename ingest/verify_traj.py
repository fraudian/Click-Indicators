"""Test the trajectory walk against ground truth, offline.

Some .gdr files in the mirror carry a `frameFixes` array of the ACTUAL per-frame x/y the recorded
player occupied, alongside the inputs. That makes them a perfect oracle: run the same integration
the mod runs, from the same anchor, and compare against the y that really happened.

If the mod's line sits below the player in game, this will show it sitting below here too - and
will say which term is wrong, instead of guessing at it.
"""
import glob
import os
import struct
import sys

# ---------- minimal msgpack reader (enough for GDR v1) ----------

class MP:
    def __init__(self, b):
        self.b = b; self.i = 0

    def u8(self):
        v = self.b[self.i]; self.i += 1; return v

    def val(self):
        c = self.u8()
        if c <= 0x7f: return c
        if c >= 0xe0: return c - 256
        if 0x80 <= c <= 0x8f: return self.map(c & 0xf)
        if 0x90 <= c <= 0x9f: return self.arr(c & 0xf)
        if 0xa0 <= c <= 0xbf: return self.str(c & 0x1f)
        if c == 0xc0: return None
        if c == 0xc2: return False
        if c == 0xc3: return True
        if c == 0xca:
            v = struct.unpack('>f', self.b[self.i:self.i+4])[0]; self.i += 4; return v
        if c == 0xcb:
            v = struct.unpack('>d', self.b[self.i:self.i+8])[0]; self.i += 8; return v
        if c == 0xcc: return self.u8()
        if c == 0xcd:
            v = struct.unpack('>H', self.b[self.i:self.i+2])[0]; self.i += 2; return v
        if c == 0xce:
            v = struct.unpack('>I', self.b[self.i:self.i+4])[0]; self.i += 4; return v
        if c == 0xcf:
            v = struct.unpack('>Q', self.b[self.i:self.i+8])[0]; self.i += 8; return v
        if c == 0xd0:
            v = struct.unpack('>b', self.b[self.i:self.i+1])[0]; self.i += 1; return v
        if c == 0xd1:
            v = struct.unpack('>h', self.b[self.i:self.i+2])[0]; self.i += 2; return v
        if c == 0xd2:
            v = struct.unpack('>i', self.b[self.i:self.i+4])[0]; self.i += 4; return v
        if c == 0xd3:
            v = struct.unpack('>q', self.b[self.i:self.i+8])[0]; self.i += 8; return v
        if c == 0xd9: return self.str(self.u8())
        if c == 0xda:
            n = struct.unpack('>H', self.b[self.i:self.i+2])[0]; self.i += 2; return self.str(n)
        if c == 0xdc:
            n = struct.unpack('>H', self.b[self.i:self.i+2])[0]; self.i += 2; return self.arr(n)
        if c == 0xdd:
            n = struct.unpack('>I', self.b[self.i:self.i+4])[0]; self.i += 4; return self.arr(n)
        if c == 0xde:
            n = struct.unpack('>H', self.b[self.i:self.i+2])[0]; self.i += 2; return self.map(n)
        if c == 0xdf:
            n = struct.unpack('>I', self.b[self.i:self.i+4])[0]; self.i += 4; return self.map(n)
        raise ValueError('msgpack byte %02x' % c)

    def str(self, n):
        s = self.b[self.i:self.i+n]; self.i += n
        return s.decode('utf-8', 'replace')

    def arr(self, n):
        return [self.val() for _ in range(n)]

    def map(self, n):
        return {self.val(): self.val() for _ in range(n)}


def load(path):
    d = open(path, 'rb').read()
    if not d or d[0] not in (0x80 | 0, ) and not (0x80 <= d[0] <= 0x8f or d[0] in (0xde, 0xdf)):
        return None
    try:
        return MP(d).val()
    except Exception:
        return None


# ---------- the test ----------

def analyse(path):
    o = load(path)
    if not isinstance(o, dict):
        return None
    fixes = o.get('frameFixes') or o.get('framefixes')
    inputs = o.get('inputs')
    if not fixes or not inputs:
        return None

    # per-frame truth: frame -> (x, y)
    pos = {}
    for f in fixes:
        if not isinstance(f, dict):
            continue
        fr = f.get('frame')
        p1 = f.get('p1') or {}
        if fr is None or 'x' not in p1 or 'y' not in p1:
            continue
        pos[fr] = (p1['x'], p1['y'])
    if len(pos) < 500:
        return None

    frames = sorted(pos)
    # inputs -> hold intervals for player 1, jump only
    ev = []
    for i in inputs:
        if not isinstance(i, dict):
            continue
        if i.get('2p'):
            continue
        if i.get('btn', 1) not in (0, 1):
            continue
        ev.append((i['frame'], bool(i.get('down'))))
    ev.sort()
    if not ev:
        return None

    # find the longest stretch where |dy/dx| is a constant 1 or 2 - i.e. a wave section
    best = None
    run = None
    for k in range(1, len(frames)):
        a, b = frames[k-1], frames[k]
        if b != a + 1:
            run = None; continue
        x0, y0 = pos[a]; x1, y1 = pos[b]
        dx = x1 - x0
        if dx <= 0.0001:
            run = None; continue
        s = abs((y1 - y0) / dx)
        isw = abs(s - 1.0) < 0.02 or abs(s - 2.0) < 0.02
        if isw:
            if run is None: run = [a, b]
            else: run[1] = b
            if best is None or (run[1] - run[0]) > (best[1] - best[0]):
                best = list(run)
        else:
            run = None
    if not best or best[1] - best[0] < 400:
        return None

    f0, f1 = best
    ax, ay = pos[f0]
    mini = abs((pos[f0+1][1] - ay) / (pos[f0+1][0] - ax)) > 1.5
    slope = 2.0 if mini else 1.0

    # held state at the anchor, from the inputs alone
    held = False
    for fr, dn in ev:
        if fr > f0: break
        held = dn

    # THE WALK the mod does: integrate y in x, flipping on every input transition
    cy = ay
    cx = ax
    h = held
    errs = []
    ei = 0
    while ei < len(ev) and ev[ei][0] <= f0:
        ei += 1
    for fr in range(f0 + 1, f1 + 1):
        while ei < len(ev) and ev[ei][0] <= fr:
            # advance to the transition's x, then flip
            tx = pos.get(ev[ei][0], (None, None))[0]
            if tx is not None and tx > cx:
                cy += (1.0 if h else -1.0) * slope * (tx - cx)
                cx = tx
            h = ev[ei][1]
            ei += 1
        px, ptrue = pos[fr]
        if px > cx:
            cy += (1.0 if h else -1.0) * slope * (px - cx)
            cx = px
        errs.append(cy - ptrue)

    if not errs:
        return None
    errs.sort()
    n = len(errs)
    return {
        'file': os.path.basename(path),
        'frames': f1 - f0,
        'mini': mini,
        'median_err': errs[n // 2],
        'p90_abs': sorted(abs(e) for e in errs)[int(n * 0.9)],
        'final_err': errs[-1] if abs(errs[-1]) > abs(errs[0]) else errs[0],
        'signed_drift': (sum(errs) / n),
    }


files = sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'blobs', '*.gdr')))
print('scanning %d .gdr blobs for wave sections with recorded positions...\n' % len(files))
ok = []
for f in files:
    try:
        r = analyse(f)
    except Exception:
        r = None
    if r:
        ok.append(r)
    if len(ok) >= 12:
        break

if not ok:
    print('no usable wave sections found')
    sys.exit(1)

print('%-30s %-7s %-6s %-11s %-10s %s' % ('file', 'frames', 'mini', 'median err', 'p90 |err|', 'mean drift'))
for r in ok:
    print('%-30s %-7d %-6s %-11.3f %-10.3f %+.3f'
          % (r['file'][:30], r['frames'], 'yes' if r['mini'] else 'no',
             r['median_err'], r['p90_abs'], r['signed_drift']))

allm = sorted(abs(r['median_err']) for r in ok)
print('\nsections: %d   median |error| across them: %.3f units' % (len(ok), allm[len(allm)//2]))
print('If these are ~0 the walk is correct and the bug is in the ANCHOR or the projection.')
print('If they grow, the walk itself is wrong.')
