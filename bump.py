"""Bump the patch version before a test build.

Geode caches an unzipped copy of the mod and decides whether to re-extract from the VERSION in
mod.json. Fifteen builds tonight all declared v1.0.23, so Geode kept loading a binary from three
days earlier no matter how carefully the .geode was replaced - and every result read off it was
about code that was not running. The version has to move whenever the binary does.
"""
import io, re, sys
import time as _time
p = 'mod.json'
s = io.open(p, encoding='utf-8').read()
m = re.search(r'"version":\s*"v(\d+)\.(\d+)\.(\d+)"', s)
maj, mnr, pat = int(m.group(1)), int(m.group(2)), int(m.group(3))
new = 'v%d.%d.%d' % (maj, mnr, pat + 1)
s = s[:m.start()] + '"version": "%s"' % new + s[m.end():]
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print(new)


# ---------------------------------------------------------------------------------------------
# The legacy licence path has an expiry, and it has to actually be removed.
#
# Until LIC_LEGACY_UNTIL, licGate() still accepts an install that has no signed grant - which is
# how 1,222 existing customers keep working until each of them is next online. That branch is also
# the entire pre-signature crack: it passes on a checksum the binary computes for itself, so anyone
# who patches the date back in gets the old behaviour whole. A signature scheme with a date-gated
# bypass still compiled in is a signature scheme with a bypass.
#
# So the build refuses to package once the date has passed and the branch is still there. Turning
# "remember to delete this" into something that cannot be forgotten is the only version of that
# instruction which survives a month.

_src = io.open('src/main.cpp', encoding='utf-8', newline='').read()

# The offline grace runs from THIS build, not from a date somebody typed weeks earlier. A fixed date
# that slips past before the build carrying it ships leaves the first release customers actually
# receive with no grace at all - which locks out anyone who is not online the moment they install it.
_GRACE_DAYS = 45
_new_until = int(_time.time()) + _GRACE_DAYS * 86400
_src2, _n = re.subn(r'(LIC_LEGACY_UNTIL\s*=\s*)\d+LL', r'\g<1>%dLL' % _new_until, _src)
if _n == 1 and _src2 != _src:
    io.open('src/main.cpp', 'w', encoding='utf-8', newline='').write(_src2)
    _src = _src2
    print('offline grace set to %s (%d days from this build)'
          % (_time.strftime('%Y-%m-%d', _time.gmtime(_new_until)), _GRACE_DAYS))
_m = re.search(r'LIC_LEGACY_UNTIL\s*=\s*(\d+)LL', _src)
if _m:
    _until = int(_m.group(1))
    # The grace moves with each build, so it can never be the thing that says "now delete this".
    # That needs a date which does not move: the point past which the legacy paths stop being a
    # migration aid and are only a bypass waiting to be switched back on.
    _REMOVE_AFTER = 1798761600      # 2027-01-01
    _par = io.open('src/parsers.cpp', encoding='utf-8', newline='').read()
    # Two branches expire on the same date and each is a bypass if it outlives it: licGate()'s
    # fallback to the checksum the binary computes for itself, and the parser's willingness to
    # read a plaintext macro when the install holds no key. Both are one constant away from
    # being switched back on, so the only real removal is deleting the code.
    _still_there = ('licNow() >= LIC_LEGACY_UNTIL' in _src) or ('g_vaultStrict' in _par)
    if _still_there and _time.time() >= _REMOVE_AFTER:
        raise SystemExit(
            'REFUSING TO BUILD.\n\n'
            'The removal deadline passed on %s and the legacy paths are still compiled in.\n'
            'checksum path when there is no signed grant. That branch is the original crack:\n'
            'it accepts a record the binary can compute for itself, so anyone who patches the\n'
            'date gets it back whole.\n\n'
            'Two things to delete:\n\n'
            '  src/main.cpp     licGate(): "if (licNow() >= LIC_LEGACY_UNTIL) return false;"\n'
            '                   becomes just "return false;", and the constant goes too.\n\n'
            '  src/parsers.cpp  the plaintext branch: "g_vaultStrict" goes, and reading a\n'
            '                   plain macro without a vault key stops being possible at all.'
            % _time.strftime('%Y-%m-%d', _time.gmtime(_REMOVE_AFTER)))
    if _still_there:
        _days = int((_REMOVE_AFTER - _time.time()) / 86400)
        print('note: the legacy licence path and the plaintext-macro path are still compiled '
              'in; both must be deleted in %d days' % _days)
