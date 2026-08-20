"""Does the mark survive the round trip, and does it actually name somebody?

Sealing the cache made a leaked macro FOLDER worthless. It did nothing about the leak that is still
open: a buyer pulling the library through the API with their own token and posting the plaintext.
From the server's side that is a customer using the product, and nothing can stop it - so instead,
every macro that leaves carries 32 bytes naming who it went to.

Three separate pieces have to agree on those bytes or the whole thing is decoration: the Worker that
appends them, the parser in the mod that strips them, and the tool that reads them back off a leaked
file. Any two of them agreeing is not enough - a layout mismatch means a leak arrives and the tool
says "no watermark on this file".

The Worker's own expression is run here through node, so this is the real thing rather than a second
guess at it.
"""
import hashlib
import json
import os
import struct
import subprocess
import sys

MAGIC = b'CIWM1'
WM_LEN = 32
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

ok = True


def check(name, good, detail=''):
    global ok
    print('%-58s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


def worker_mark(vault_key, macro_key, body, at):
    """Runs the Worker's own watermark arithmetic, in node, over WebCrypto."""
    js = r'''
const { webcrypto: crypto } = require("node:crypto");
const enc = new TextEncoder();
const [vault, key, bodyHex, at] = process.argv.slice(-4);
(async () => {
  const WM_MAGIC = [0x43, 0x49, 0x57, 0x4d, 0x31];
  const WM_LEN = 32;
  const digest = await crypto.subtle.digest("SHA-256", enc.encode(vault + "" + key));
  const src = Buffer.from(bodyHex, "hex");
  const out = new Uint8Array(src.length + WM_LEN);
  out.set(src, 0);
  const t = new Uint8Array(WM_LEN);
  t.set(WM_MAGIC, 0);
  t.set(new Uint8Array(digest).slice(0, 16), 5);
  new DataView(t.buffer).setBigInt64(21, BigInt(Number(at)), true);
  out.set(t, src.length);
  process.stdout.write(Buffer.from(out).toString("hex"));
})();
'''
    r = subprocess.run([_node(), '-e', js, '--', vault_key, macro_key, body.hex(), str(at)],
                       capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        sys.exit('node failed: ' + (r.stderr or r.stdout))
    return bytes.fromhex(r.stdout.strip())


def _node():
    import shutil
    n = shutil.which('node')
    if not n:
        sys.exit('node not found - needed to run the Worker\'s own code')
    return n


def read_tag(blob):
    """What server/find-leaker.py does."""
    if len(blob) < WM_LEN + 4:
        return None, None
    tail = blob[-WM_LEN:]
    if tail[:5] != MAGIC:
        return None, None
    return tail[5:21], struct.unpack('<q', tail[21:29])[0]


def strip(blob):
    """What parseMacroFileInner does before handing the bytes to a format parser."""
    if len(blob) > WM_LEN + 4 and blob[-WM_LEN:][:5] == MAGIC:
        return blob[:-WM_LEN]
    return blob


VAULT_A = 'FaKeVaultKeyForAccountA-000000000000000000='
VAULT_B = 'FaKeVaultKeyForAccountB-111111111111111111='
KEY = 'abc123slug'
AT = 1800000000
BODY = bytes((i * 37 + 11) & 0xff for i in range(2048))

marked = worker_mark(VAULT_A, KEY, BODY, AT)

# --- 1. the mark does not disturb the macro -------------------------------------------------------
check('the marked file is the macro plus 32 bytes', len(marked) == len(BODY) + WM_LEN,
      '%d -> %d' % (len(BODY), len(marked)))
check('  and the macro itself is untouched', marked[:len(BODY)] == BODY)
check('  so the parser gets back exactly what was served', strip(marked) == BODY)

# --- 2. the tool reads it, and reads the same thing the Worker wrote -------------------------------
tag, served = read_tag(marked)
check('the tool finds a watermark', tag is not None)
check('  and the time it was served', served == AT, str(served))
expect = hashlib.sha256((VAULT_A + '\x1f' + KEY).encode()).digest()[:16]
check('  the tag is what find-leaker recomputes from the account', tag == expect,
      tag.hex() if tag else '')

# --- 3. it names ONE account -----------------------------------------------------------------------
other = hashlib.sha256((VAULT_B + '\x1f' + KEY).encode()).digest()[:16]
check('a different account gets a different tag', other != expect)
elsewhere = hashlib.sha256((VAULT_A + '\x1f' + 'a-different-macro').encode()).digest()[:16]
check('  and the same account gets a different one per macro', elsewhere != expect,
      'so a tag lifted from one file cannot be pinned to another')

# --- 4. it never accuses anyone by accident --------------------------------------------------------
check('an unmarked file reports no watermark', read_tag(BODY)[0] is None)
check('  and is left alone by the parser', strip(BODY) == BODY)
tail_looks_like_magic = BODY[:-5] + MAGIC
check('  a file merely ENDING in the magic is still 32 bytes short of a mark',
      read_tag(tail_looks_like_magic)[0] is None)
short = MAGIC + b'\0' * 27
check('  and a 32-byte file is not treated as a bare mark', read_tag(short)[0] is None)

# --- 5. the mark survives the seal, which is the point ---------------------------------------------
# The mod seals whatever the server sent, mark included, so the plaintext inside the vault carries
# it. Anything that gets a plaintext macro out of the mod gets a marked one.
check('what the mod seals is the marked bytes, not the bare macro',
      strip(marked) == BODY and marked[len(BODY):len(BODY) + 5] == MAGIC,
      'the mark rides inside the vault and comes out with the file')


# --- 6. the build itself ---------------------------------------------------------------------------
# A Discord that redistributes cracked builds has a channel named after this mod, and what it passes
# around is the BINARY. So every .geode handed out carries the same 32 bytes, in the zip's trailing
# comment field - which every zip reader ignores, including the one Geode installs with.
#
# The thing that must not break: the marked file has to still be a valid, installable archive. This
# runs the Worker's own zipSetComment over the real build and then opens the result.
import zipfile

GEODE = os.path.join(ROOT, 'build', 'jackz.click-indicators.geode')


def worker_zip_comment(path, comment):
    """Runs the Worker's own zipSetComment, in node, over the real .geode."""
    js = r"""
const fs = require("node:fs");
const [inPath, outPath, commentHex] = process.argv.slice(-3);
function zipSetComment(buf, comment) {
  const b = new Uint8Array(buf);
  let eocd = -1;
  const from = Math.max(0, b.length - 66000);
  for (let i = b.length - 22; i >= from; i--) {
    if (b[i] === 0x50 && b[i+1] === 0x4b && b[i+2] === 0x05 && b[i+3] === 0x06) { eocd = i; break; }
  }
  if (eocd < 0) return null;
  const out = new Uint8Array(eocd + 22 + comment.length);
  out.set(b.subarray(0, eocd + 22), 0);
  out.set(comment, eocd + 22);
  out[eocd + 20] = comment.length & 0xff;
  out[eocd + 21] = (comment.length >> 8) & 0xff;
  return out;
}
const src = fs.readFileSync(inPath);
const out = zipSetComment(src, Buffer.from(commentHex, "hex"));
if (!out) { process.stderr.write("no EOCD"); process.exit(1); }
fs.writeFileSync(outPath, Buffer.from(out));
"""
    out = path + '.marked'
    r = subprocess.run([_node(), '-e', js, '--', path, out, comment.hex()],
                       capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        sys.exit('node failed: ' + (r.stderr or r.stdout))
    return out


if not os.path.exists(GEODE):
    print('%-58s SKIP (no build/jackz.click-indicators.geode)' % 'the built .geode carries the mark')
else:
    trailer = bytearray(WM_LEN)
    trailer[0:5] = MAGIC
    trailer[5:21] = hashlib.sha256((VAULT_A + '\x1f' + 'geode:v1.0.70').encode()).digest()[:16]
    trailer[21:29] = struct.pack('<q', AT)
    marked_path = worker_zip_comment(GEODE, bytes(trailer))
    try:
        orig = open(GEODE, 'rb').read()
        got = open(marked_path, 'rb').read()
        check('marking the build adds exactly 32 bytes', len(got) == len(orig) + WM_LEN,
              '%d -> %d' % (len(orig), len(got)))
        # The one that matters: it still has to install.
        with zipfile.ZipFile(marked_path) as z:
            names = z.namelist()
            bad = z.testzip()
            modjson = json.loads(z.read('mod.json'))
        check('  and it is still a readable archive', bad is None and len(names) > 1,
              '%d entries, no corrupt member' % len(names))
        check('  with the same contents as before', modjson.get('id') == 'jackz.click-indicators',
              'mod.json reads back as %s %s' % (modjson.get('id'), modjson.get('version')))
        with zipfile.ZipFile(GEODE) as z0, zipfile.ZipFile(marked_path) as z1:
            same = all(z0.read(nm) == z1.read(nm) for nm in z0.namelist())
        check('  every file inside is byte-identical', same,
              'the mod that comes out is the mod that went in')
        tag2, served2 = read_tag(got)
        check('  the mark is where find-leaker looks for it', tag2 == bytes(trailer[5:21]),
              tag2.hex() if tag2 else 'not found')
        check('  and it names the account', tag2 == hashlib.sha256(
            (VAULT_A + '\x1f' + 'geode:v1.0.70').encode()).digest()[:16])
        other_ver = hashlib.sha256((VAULT_A + '\x1f' + 'geode:v1.0.69').encode()).digest()[:16]
        check('  a mark from another release cannot be pinned to this one',
              other_ver != tag2)
    finally:
        try:
            os.remove(marked_path)
        except OSError:
            pass

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
