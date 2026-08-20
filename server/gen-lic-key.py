"""Generate the licence signing keypair.

Run ONCE. The private key never leaves this machine except into a Cloudflare secret; the public key
goes into the mod binary, where it is harmless - it can verify a grant and cannot produce one. That
asymmetry is the whole point of the change: the old licGate() recomputed a checksum the binary
already knew how to compute, so a crack needed no code patch at all, just four values in the save
file. There is no constant you can put in a binary that lets it VERIFY without also letting it
FORGE - unless the constant is a public key.

    python server/gen-lic-key.py

Writes  server/.lic-signing-key   (private, gitignored - never commit, never paste)
Prints  the public key, as a C array to paste into src/main.cpp

Then, once:

    wrangler secret put LIC_SIGN_KEY --config server/wrangler.jsonc  < server/.lic-signing-key

Rotating it later invalidates every grant in the field at once, so every customer has to be online
before their next launch works. Do it only for a compromise, and post about it first.
"""
import base64
import os
import stat
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '.lic-signing-key')

if os.path.exists(OUT):
    sys.exit('%s already exists. Refusing to overwrite it - a new key signs out every customer '
             'until they reconnect. Delete it deliberately if that is what you want.' % OUT)

priv = Ed25519PrivateKey.generate()
pub = priv.public_key()

pkcs8 = priv.private_bytes(
    encoding=serialization.Encoding.DER,
    format=serialization.PrivateFormat.PKCS8,
    encryption_algorithm=serialization.NoEncryption(),
)
raw_pub = pub.public_bytes(
    encoding=serialization.Encoding.Raw,
    format=serialization.PublicFormat.Raw,
)

# Written, never printed. Anything that reaches a terminal reaches a scrollback, a screenshot and a
# support thread.
fd = os.open(OUT, os.O_WRONLY | os.O_CREAT | os.O_EXCL, stat.S_IRUSR | stat.S_IWUSR)
with os.fdopen(fd, 'w') as f:
    f.write(base64.b64encode(pkcs8).decode())

print('private key written to server/.lic-signing-key (not printed, not committed)')
print()
print('public key, for src/main.cpp:')
print()
print('static const unsigned char kLicPub[32] = {')
for i in range(0, 32, 8):
    row = ', '.join('0x%02x' % b for b in raw_pub[i:i + 8])
    print('    %s,' % row)
print('};')
print()
print('public key hex: %s' % raw_pub.hex())
print()
print('next:  wrangler secret put LIC_SIGN_KEY --config server/wrangler.jsonc < server/.lic-signing-key')
