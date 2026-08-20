#!/usr/bin/env python
"""Create an account, or set a new password on one that exists.

    python server/make-account.py holasamirxd123@gmail.com someone@else.com

Writes the SQL and prints the wrangler command to apply it, plus the generated passwords.
Nothing reaches the database from here - you run the command it prints, so you can read it first.
Same convention as grant-licence.py.

The hash has to match what the Worker computes at sign-in or the account exists but nobody can
get into it. The Worker does PBKDF2-HMAC-SHA256 twice over, feeding the first round's 32 bytes
back in as the password material for the second, 100,000 iterations each, and stores standard
base64 of the result. This reproduces that exactly - and the caller should still prove it by
signing in through /api/login afterwards rather than trusting that it lines up.

Passwords avoid 0/O and 1/l/I, because these get read off a screen and typed into Geometry Dash's
own text box, which only accepts printable ASCII in the first place.
"""
import base64
import hashlib
import os
import re
import secrets
import sys
import time
import uuid

PBKDF2_ITERS = 100_000
PBKDF2_ROUNDS = 2
ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789"
PW_LEN = 16

VALID = re.compile(r"^[^@\s]+@[^@\s.]+\.[^@\s]+$")


def q(s):
    return "'" + str(s).replace("'", "''") + "'"


def gen_password():
    return "".join(secrets.choice(ALPHABET) for _ in range(PW_LEN))


def hash_password(password, salt_b64):
    """Byte-for-byte equivalent of hashPassword() in worker.js."""
    salt = base64.b64decode(salt_b64)
    material = password.encode("utf-8")
    for _ in range(PBKDF2_ROUNDS):
        material = hashlib.pbkdf2_hmac("sha256", material, salt, PBKDF2_ITERS, 32)
    return base64.b64encode(material).decode("ascii")


def main(argv):
    emails = [a.strip().lower() for a in argv]
    if not emails:
        print(__doc__, file=sys.stderr)
        return 2
    for e in emails:
        if not VALID.match(e):
            print("that does not look like an email address: " + e, file=sys.stderr)
            return 2

    now = int(time.time())
    stmts, creds = [], []
    for email in emails:
        pw = gen_password()
        salt = base64.b64encode(os.urandom(16)).decode("ascii")
        pwh = hash_password(pw, salt)
        uid = str(uuid.uuid4())
        creds.append((email, pw))
        # One statement pair per address so this is safe whether or not the account exists.
        # INSERT OR IGNORE leaves an existing row alone; the UPDATE then sets the password on
        # whichever row is there. Doing it the other way round would create a second row for an
        # address that already had one, and email is UNIQUE, so the whole batch would fail.
        stmts.append(
            "INSERT OR IGNORE INTO users (id, email, pw_hash, pw_salt, paid, created)\n"
            "  VALUES (%s, %s, %s, %s, 0, %d);" % (q(uid), q(email), q(pwh), q(salt), now)
        )
        stmts.append(
            "UPDATE users SET pw_hash = %s, pw_salt = %s WHERE email = %s;"
            % (q(pwh), q(salt), q(email))
        )
        # A password change should not leave old game sessions running on it.
        stmts.append(
            "DELETE FROM devices WHERE user_id = (SELECT id FROM users WHERE email = %s);"
            % q(email)
        )

    sql = "\n".join(stmts)
    out = os.path.join(os.environ.get("TEMP", "/tmp"), "ci-accounts.sql")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(sql + "\n")

    print()
    print("  accounts: %d" % len(emails))
    print()
    for email, pw in creds:
        print("    %-32s  %s" % (email, pw))
    print()
    print("Apply it with:")
    print("  npx wrangler d1 execute clickindicators --remote -c server/wrangler.jsonc --file=" + out)
    print()
    print("Then PROVE it worked - an account nobody can sign into is worse than no account:")
    print("  curl -s -X POST https://clickindicatorsmod.com/api/mod/login \\")
    print("    -H 'content-type: application/json' \\")
    print("    -d '{\"email\":\"...\",\"password\":\"...\",\"device\":\"check\"}'")
    print()
    print("These accounts are NOT paid. Give them the licence separately:")
    print("  python server/grant-licence.py grant <email> \"<why>\"")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
