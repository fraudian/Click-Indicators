#!/usr/bin/env python
"""Give someone the mod for free, or take it back.

    python tools/grant-licence.py grant  their@email.com "creator: tiktok @handle"
    python tools/grant-licence.py revoke their@email.com

Writes the SQL and prints the wrangler command to apply it. Nothing is sent to the database
by this script - you run the command it gives you, so you can read it first.

A grant writes a purchases row for 0, not just paid = 1. That matters: an account marked paid
with no purchase behind it is indistinguishable from a payment that half-failed, and one of
those has already turned up in this database and cost an evening working out which it was.
Every free key is therefore recorded as a deliberate 0 with a reason attached.
"""
import re
import sys
import os
import time

VALID = re.compile(r"^[^@\s]+@[^@\s.]+\.[^@\s]+$")


def q(s: str) -> str:
    return "'" + str(s).replace("'", "''") + "'"


def main() -> int:
    if len(sys.argv) < 3 or sys.argv[1] not in ("grant", "revoke"):
        print(__doc__, file=sys.stderr)
        return 2
    action, email = sys.argv[1], sys.argv[2].strip().lower()
    reason = sys.argv[3] if len(sys.argv) > 3 else "creator programme"
    if not VALID.match(email):
        print("that does not look like an email address: " + email, file=sys.stderr)
        return 2

    now = int(time.time())
    if action == "grant":
        pid = "comp-%s-%d" % (re.sub(r"[^a-z0-9]+", "", email.split("@")[0])[:16], now)
        sql = "\n".join([
            "UPDATE users SET paid = 1 WHERE email = %s;" % q(email),
            # Recorded as a real row so paid accounts and purchase records always reconcile.
            "INSERT OR REPLACE INTO purchases (id, user_id, amount, currency, status, created)",
            "  SELECT %s, id, 0, 'usd', %s, %d FROM users WHERE email = %s;"
            % (q(pid), q(reason[:60]), now, q(email)),
        ])
        after = ("They can download immediately. Tell them to sign in inside the game with the\n"
                 "same email and password - the licence is on the account, not the file.")
    else:
        sql = "\n".join([
            "UPDATE users SET paid = 0 WHERE email = %s;" % q(email),
            # The mod stops at its next licence check, within about half an hour of play.
            "DELETE FROM devices WHERE user_id = (SELECT id FROM users WHERE email = %s);" % q(email),
        ])
        after = ("The mod stops working for them at its next check, within ~30 minutes of play.\n"
                 "Their account and any purchase history stay intact.")

    out = os.path.join(os.environ.get("TEMP", "/tmp"), "ci-licence.sql")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(sql + "\n")

    print()
    print("  %s: %s" % (action, email))
    if action == "grant":
        print("  reason  : %s" % reason)
    print()
    print(sql)
    print()
    print("Apply it with:")
    print("  npx wrangler d1 execute clickindicators --remote -c server/wrangler.jsonc --file=" + out)
    print()
    print("Check it took:")
    print("  npx wrangler d1 execute clickindicators --remote -c server/wrangler.jsonc \\")
    print("    --command \"SELECT email, paid FROM users WHERE email = %s\"" % q(email))
    print()
    print(after)
    print()
    print("If the account does not exist yet, nothing happens - both statements match on email.")
    print("Have them sign up first, then run this.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
