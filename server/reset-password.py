#!/usr/bin/env python
"""Reset a customer's password by hand.

    python server/reset-password.py their@email.com

Generates a temporary password, prints it once, and writes the SQL to apply it. There is no
self-service reset yet and no mailer configured, so this is the only way to get someone back
into an account they have paid for.

The hash must match worker.js exactly or the new password silently will not work:
PBKDF2-SHA256, 100,000 iterations, TWO chained rounds (Workers caps a single call at 100k),
32-byte output, base64. The salt is base64 of 16 random bytes.

Also clears the rate-limit rows keyed on the address. Note those are not what actually locks
anyone out - checkLock reads the IP-keyed rows, which this cannot know - so a customer who has
been guessing may still have to wait, or simply sign in successfully, which now clears it.
"""
import base64
import hashlib
import os
import re
import secrets
import sys

ITERS = 100_000
ROUNDS = 2
# No l/I/1/O/0 - these get read aloud or retyped and confused constantly.
ALPHABET = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789"


def hash_password(password: str, salt_b64: str) -> str:
    salt = base64.b64decode(salt_b64)
    material = password.encode("utf-8")
    for _ in range(ROUNDS):
        material = hashlib.pbkdf2_hmac("sha256", material, salt, ITERS, dklen=32)
    return base64.b64encode(material).decode()


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    email = sys.argv[1].strip().lower()
    if not re.match(r"^[^@\s]+@[^@\s.]+\.[^@\s]+$", email):
        print("that does not look like an email address: " + email, file=sys.stderr)
        return 2

    # 14 chars from a 56-char alphabet is ~81 bits. Long enough that handing it out over
    # Discord is not the weak link, short enough to retype without mistakes.
    password = "".join(secrets.choice(ALPHABET) for _ in range(14))
    salt_b64 = base64.b64encode(os.urandom(16)).decode()
    pw_hash = hash_password(password, salt_b64)

    def q(s: str) -> str:
        return "'" + s.replace("'", "''") + "'"

    sql = "\n".join([
        "UPDATE users SET pw_hash = %s, pw_salt = %s WHERE email = %s;"
        % (q(pw_hash), q(salt_b64), q(email)),
        # A customer who forgot their password has usually tripped the backoff already.
        "DELETE FROM login_attempts WHERE ident IN (%s, %s);"
        % (q("web:" + email), q("mod:" + email)),
    ])

    out = os.path.join(os.environ.get("TEMP", "/tmp"), "ci-reset.sql")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(sql + "\n")

    print()
    print("  account   : " + email)
    print("  password  : " + password)
    print()
    print("Apply it with:")
    print("  npx wrangler d1 execute clickindicators --remote -c server/wrangler.jsonc --file="
          + out)
    print()
    print("Then delete that file - it contains the hash - and send the password to the")
    print("customer. It does not expire and they cannot change it themselves yet, so treat")
    print("it as their real password.")
    print()
    print("Their IP may still be rate limited for up to 30 minutes. That is separate from")
    print("the address and clears on its own.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
