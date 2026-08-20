"""Who was this macro served to?

Every macro the API hands out carries 32 bytes on the end naming the account it went to. Not the
account id - a tag derived from that account's vault key - so a leaked file identifies the buyer to
you and to nobody else, and cannot be forged onto someone innocent.

    python server/find-leaker.py <leaked-file> [--key <macro-key>]

The macro key is the slug the file was fetched by: the `f=` parameter for a hyperbolus macro, or the
r2_key for a mirrored one. Without it the tag cannot be recomputed, so pass it - it is in the URL the
mod used, and in the filename for mirrored macros.

Reads (id, email, vault_key) from D1 through wrangler, computes the expected tag for every account,
and reports the one that matches.

What it cannot do: identify a file whose last 32 bytes were removed. Anyone who compares two
accounts' copies of the same macro can see them differ and cut them off. This catches the ordinary
case - one buyer posting a zip - and that is what it is for.
"""
import argparse
import hashlib
import json
import struct
import subprocess
import sys

MAGIC = b'CIWM1'
WM_LEN = 32


def read_tag(path):
    blob = open(path, 'rb').read()
    if len(blob) < WM_LEN + 4:
        sys.exit('%s is too small to carry a mark.' % path)
    tail = blob[-WM_LEN:]
    if tail[:5] != MAGIC:
        sys.exit('No watermark on this file.\n'
                 'Either it was never served by the API - a recording, or a file the player '
                 'imported themselves - or the last 32 bytes were removed.')
    tag = tail[5:21]
    served = struct.unpack('<q', tail[21:29])[0]
    return tag, served


def geode_key(path):
    """A .geode is a zip. Read the version out of its mod.json rather than asking for it."""
    try:
        import zipfile
        with zipfile.ZipFile(path) as z:
            return 'geode:' + json.loads(z.read('mod.json'))['version']
    except Exception:
        return None


def accounts():
    sql = "SELECT id, email, vault_key FROM users WHERE vault_key IS NOT NULL"
    out = subprocess.run(
        ['wrangler', 'd1', 'execute', 'clickindicators', '--remote', '--json',
         '--config', 'server/wrangler.jsonc', '--command', sql],
        capture_output=True, text=True, shell=True)
    if out.returncode != 0:
        sys.exit('wrangler failed:\n' + (out.stderr or out.stdout))
    # wrangler prints a banner before the JSON on some versions.
    body = out.stdout[out.stdout.index('['):]
    data = json.loads(body)
    rows = []
    for chunk in data:
        rows.extend(chunk.get('results', []))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--key',
                    help="the slug the macro was fetched by (the f= parameter, or the r2_key). "
                         "Worked out automatically for a .geode.")
    a = ap.parse_args()

    # A .geode carries its mark in the zip's trailing comment, which is the end of the file - so the
    # same reader finds it. The key is the release it was built from, and the archive says which.
    if not a.key:
        a.key = geode_key(a.file)
        if not a.key:
            sys.exit('Pass --key: the slug this macro was fetched by. It is in the URL the mod '
                     'used, and the tag covers it as well as the account, so guessing never '
                     'matches anybody.')
        print('this is a .geode - keying on %s' % a.key)

    tag, served = read_tag(a.file)
    import datetime
    when = datetime.datetime.fromtimestamp(served, datetime.timezone.utc).isoformat()
    print('watermark : %s' % tag.hex())
    print('served at : %s' % when)
    print()

    rows = accounts()
    print('checking %d accounts...' % len(rows))
    for r in rows:
        want = hashlib.sha256(
            (r['vault_key'] + '\x1f' + a.key).encode('utf-8')).digest()[:16]
        if want == tag:
            print()
            print('MATCH: %s  (account %s)' % (r['email'], r['id']))
            print()
            print('To revoke, on the website: mark the account unpaid and remove its device.')
            print('Their macros stop working at the end of the grant they are holding, and they')
            print('cannot download or decrypt any more.')
            return
    print()
    print('No match. Either the account has been deleted, or the --key is not the one this file')
    print('was fetched by - the tag is over the key as well as the account, so a wrong key never')
    print('matches anybody.')


if __name__ == '__main__':
    main()
