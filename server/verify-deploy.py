"""Did the deploy actually work?

Everything built over the last few days depends on fields the server did not use to send. The crypto
is cross-checked offline - RFC vectors, and a grant signed by the Worker's own code path - but the
WIRE has never been exercised: whether /api/mod/check really returns a grant, whether the vault
column got migrated, whether a downloaded macro carries its trailer.

All of it degrades quietly when it is missing, which is right for customers and terrible for finding
out. A build with no grant falls back to the legacy path; a mod with no vault key writes plain files.
Nothing breaks, nothing complains, and the protection simply is not there. This is the thing that
says so out loud.

    python server/verify-deploy.py --email you@example.com

Asks for the password at the prompt - it is never passed on the command line, where it would land in
your shell history.

Checks, in order:
  1. sign-in returns a grant, and the grant verifies against the public key in the mod
  2. the grant is bound to the token that was issued, not to some other one
  3. sign-in returns a vault key, so macros will be sealed rather than written in the clear
  4. a re-check returns a FRESH grant, so a licence in daily use never nears its expiry
  5. a downloaded macro carries a watermark, and it is the one belonging to this account
  6. the .geode download carries one too, and is still a valid archive afterwards
"""
import argparse
import base64
import getpass
import hashlib
import io
import json
import struct
import sys
import urllib.error
import urllib.request
import zipfile

API = 'https://clickindicatorsmod.com/api'
WM_MAGIC = b'CIWM1'
WM_LEN = 32

# The same key that is compiled into the mod. Public by design: it verifies a grant and cannot
# produce one. If this ever disagrees with src/main.cpp, the deploy is signing with a key the mod
# does not trust and every customer is about to be signed out.
PUB_HEX = 'ad8572fd0133e2f23fa5ed4284380a5f019cc0fc7e783f8f7bef3624c369d919'

failures = 0


def check(name, ok, detail=''):
    global failures
    print('%-58s %s %s' % (name, 'PASS' if ok else '** FAIL **', detail))
    if not ok:
        failures += 1
    return ok


def post(path, body, token=None):
    req = urllib.request.Request(
        API + path, data=json.dumps(body).encode(),
        headers={'content-type': 'application/json', 'user-agent': 'ClickGuide/1.0'})
    if token:
        req.add_header('Authorization', 'Bearer ' + token)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read())
        except Exception:
            return e.code, {}


def get(path, token=None):
    req = urllib.request.Request(API + path, headers={'user-agent': 'ClickGuide/1.0'})
    if token:
        req.add_header('Authorization', 'Bearer ' + token)
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.status, dict(r.headers), r.read()


def b64u_decode(s):
    return base64.urlsafe_b64decode(s + '=' * (-len(s) % 4))


def verify_grant(grant, token):
    """The mod's grantVerify, in Python. Same layout, same order of checks."""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.exceptions import InvalidSignature
    if '.' not in grant:
        return None, 'no dot - not a grant'
    p, sig = grant.split('.', 1)
    payload, signature = b64u_decode(p), b64u_decode(sig)
    if len(payload) != 56 or len(signature) != 64:
        return None, 'wrong size: %d byte payload, %d byte signature' % (len(payload), len(signature))
    if payload[:4] != b'CIG1':
        return None, 'bad magic'
    try:
        Ed25519PublicKey.from_public_bytes(bytes.fromhex(PUB_HEX)).verify(signature, payload)
    except InvalidSignature:
        return None, 'SIGNATURE DOES NOT VERIFY - the Worker is signing with a different key'
    bind = hashlib.sha512(token.encode()).digest()[:32]
    if payload[12:44] != bind:
        return None, 'not bound to this token'
    exp = struct.unpack('<q', payload[4:12])[0]
    iat = struct.unpack('<q', payload[44:52])[0]
    return (iat, exp), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--email', required=True)
    ap.add_argument('--macro', help='a macro slug to test the download with (the f= parameter)')
    a = ap.parse_args()
    pw = getpass.getpass('Password for %s (not echoed, not stored): ' % a.email)

    print()
    code, j = post('/mod/login', {'email': a.email, 'password': pw, 'device': 'deploy-check'})
    if not check('sign-in works', code == 200 and j.get('ok'),
                 j.get('error', 'HTTP %s' % code)):
        sys.exit(1)
    token = j.get('token', '')

    grant = j.get('grant')
    if check('  it returns a signed grant', bool(grant),
             'MISSING - is LIC_SIGN_KEY set? wrangler secret put LIC_SIGN_KEY'):
        info, err = verify_grant(grant, token)
        check('  the grant verifies against the key in the mod', info is not None, err or '')
        if info:
            days = (info[1] - info[0]) / 86400.0
            check('  and it lasts about three weeks', 20 <= days <= 22, '%.1f days' % days)

    vault = j.get('vault')
    check('  it returns a vault key', bool(vault),
          'MISSING - did migrate-vault.sql run? macros will be written in the clear')

    code, j2 = post('/mod/check', {'token': token})
    check('re-checking the licence works', code == 200 and j2.get('valid'), j2.get('error', ''))
    g2 = j2.get('grant')
    check('  and returns a fresh grant', bool(g2) and g2 != grant,
          'a licence in daily use must never approach its expiry')
    check('  and the vault key is the same one', j2.get('vault') == vault,
          'a changing key would make every cached macro unreadable')

    if a.macro:
        code, hdr, body = get('/macro?f=' + a.macro, token)
        check('a macro downloads', code == 200 and len(body) > WM_LEN)
        tail = body[-WM_LEN:]
        if check('  and carries a watermark', tail[:5] == WM_MAGIC,
                 'MISSING - leaked macros will not name anybody'):
            want = hashlib.sha256((vault + '\x1f' + a.macro).encode()).digest()[:16]
            check('  which names this account', tail[5:21] == want, tail[5:21].hex())
    else:
        print('%-58s SKIP (pass --macro <slug>)' % 'a macro download carries a watermark')

    code, hdr, body = get('/mod/download', token)
    if check('the build downloads', code == 200 and len(body) > 1000):
        tail = body[-WM_LEN:]
        marked = tail[:5] == WM_MAGIC
        check('  and carries a watermark', marked,
              'MISSING - cracked builds will not name anybody')
        try:
            with zipfile.ZipFile(io.BytesIO(body)) as z:
                ver = json.loads(z.read('mod.json'))['version']
                bad = z.testzip()
            check('  and is still a valid, installable archive', bad is None, 'version ' + ver)
            if marked:
                want = hashlib.sha256((vault + '\x1f' + 'geode:' + ver).encode()).digest()[:16]
                check('  and the mark names this account', tail[5:21] == want, tail[5:21].hex())
        except Exception as e:
            check('  and is still a valid, installable archive', False, str(e))
        check('  the sha256 header matches what came down the wire',
              hdr.get('x-ci-sha256', '') == hashlib.sha256(body).hexdigest(),
              'the page would be quoting a hash nobody can reproduce')

    print()
    if failures:
        print('%d CHECK(S) FAILED - the protection is not fully live.' % failures)
        sys.exit(1)
    print('ALL CHECKS PASS - signed grants, sealed macros and both watermarks are live.')
    print()
    print('This account is now signed in on "deploy-check" and every other device is signed out.')
    print('Sign in again on your own PC when you are done.')


if __name__ == '__main__':
    main()
