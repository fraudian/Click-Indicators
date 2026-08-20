#!/usr/bin/env python3
"""
Stage 3 of the t.me/gdmacros mirror: push blobs to R2 and the index to D1.

SECURITY NOTE, because this is the one place where a mistake would be expensive.

Every text column here - level_name, credit, showcase, filename - is scraped from a Telegram
caption, and ANYONE can put text into that channel through @GDmacrosBot. `wrangler d1 execute
--file` runs multi-statement SQL with full account privilege against the same database that holds
users.paid and devices.token_hash. So a level name containing a quote and a semicolon would not be
a display bug, it would be arbitrary SQL - an attacker could mint themselves a paid device row and
walk straight through the licence gate that the whole server-side redesign exists to enforce.

Nothing scraped is ever interpolated into SQL as text. Every string is emitted as a SQLite hex
literal, CAST(x'..' AS TEXT), which has no quoting to escape and therefore cannot terminate a
statement no matter what it contains. Every number goes through int()/float(). r2_key is rebuilt
here from the sha256 rather than carried through from work.db.

Usage:
    python sync_d1.py --upload      # push blobs to R2 (resumable, safe to re-run)
    python sync_d1.py --sql         # write the D1 index SQL, print the command to apply it
    python sync_d1.py --approve     # flip serve=1 for reviewed rows (see below)
"""

import argparse
import os
import re
import sqlite3
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(HERE, "work.db")
BUCKET = "clickindicators-macros"
D1 = "clickindicators"
WRANGLER_CFG = os.path.join(os.path.dirname(HERE), "server", "wrangler.jsonc")
OUT_SQL = os.path.join(HERE, "tg-index.sql")

SHA_RE = re.compile(r"^[0-9a-f]{64}$")
FMT_OK = ("gdr2", "slc", "gdr")


def sqltext(s, limit=120):
    """A scraped string as a hex literal. There is no quote to escape, so no caption can end the
    statement it sits in. Control characters are dropped and the value is length-capped, because a
    display column has no business carrying either."""
    s = "".join(ch for ch in (s or "") if ch >= " ")[:limit]
    if not s:
        return "NULL"
    return "CAST(x'" + s.encode("utf-8").hex() + "' AS TEXT)"


def wrangler(args, timeout=120):
    cmd = ["npx", "wrangler"] + args
    # Decode explicitly. text=True uses the console codepage, and wrangler echoes filenames that
    # are not representable in cp1252 - which raised out of the middle of a 1,855-object run.
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                          encoding="utf-8", errors="replace", shell=(os.name == "nt"))


def upload(c, workers=6):
    rows = c.execute(
        "SELECT msg_id, sha256, format, local FROM posts WHERE state='mirrored' AND sha256 IS NOT NULL"
    ).fetchall()
    todo = [r for r in rows if r[3] and os.path.exists(r[3])]
    if not todo:
        print("nothing to upload - run mirror_telegram.py first")
        return

    print("uploading %d objects to r2://%s\n" % (len(todo), BUCKET))
    done = {"n": 0, "fail": 0}

    def put(row):
        msg_id, sha, fmt, path = row
        if not SHA_RE.match(sha or "") or fmt not in FMT_OK:
            done["fail"] += 1
            return msg_id, False
        key = "tg/%s.%s" % (sha, fmt)
        r = wrangler(["r2", "object", "put", "%s/%s" % (BUCKET, key),
                      "--file", path, "--remote"])
        ok = r.returncode == 0
        done["n" if ok else "fail"] += 1
        if done["n"] and done["n"] % 50 == 0:
            print("  %d/%d uploaded" % (done["n"], len(todo)))
        if not ok:
            print("  ! %s: %s" % (key, (r.stderr or "").strip()[:120]))
        return msg_id, ok

    # as_completed, not ex.map: map re-raises the first exception when the results are consumed and
    # would throw away the bookkeeping for every object that DID upload in this run, so a single
    # transient failure would make the next run re-upload everything.
    from concurrent.futures import as_completed
    results = []
    with ThreadPoolExecutor(max_workers=workers) as ex:
        futs = [ex.submit(put, r) for r in todo]
        for fu in as_completed(futs):
            try:
                results.append(fu.result())
            except Exception as e:                  # noqa: BLE001 - one bad object must not lose the rest
                done["fail"] += 1
                print("  ! upload worker failed: %s" % str(e)[:120])

    for msg_id, ok in results:
        if ok:
            c.execute("UPDATE posts SET state='uploaded' WHERE msg_id=?", (msg_id,))
    c.commit()
    print("\nuploaded %d, failed %d" % (done["n"], done["fail"]))


def emit_sql(c, approve_backfill):
    rows = c.execute(
        "SELECT msg_id, level_id, format, sha256, bytes, filename, name, credit, showcase, "
        "fps, inputs, verified, posted FROM posts WHERE state='uploaded' ORDER BY msg_id"
    ).fetchall()
    if not rows:
        print("nothing to sync - run --upload first")
        return

    now = int(time.time())
    lines = ["-- generated by ingest/sync_d1.py - do not hand-edit",
             "-- every scraped string is a hex literal; nothing here is interpolated text"]
    kept = 0
    for (msg_id, level_id, fmt, sha, nbytes, filename, name, credit, showcase,
         fps, inputs, verified, posted) in rows:
        # Re-validate at the boundary rather than trusting work.db, which is a local file that
        # something else could have touched.
        if not SHA_RE.match(sha or "") or fmt not in FMT_OK:
            continue
        if not (isinstance(level_id, int) and 10 <= level_id <= 999999999999):
            continue
        r2_key = "tg/%s.%s" % (sha, fmt)          # rebuilt here, never carried
        serve = 1 if approve_backfill else 0
        lines.append(
            "INSERT OR REPLACE INTO tg_macros (msg_id, level_id, format, r2_key, bytes, filename, "
            "level_name, credit, showcase, fps, inputs, verified, serve, posted, added) VALUES "
            "(%d, %d, %s, %s, %d, %s, %s, %s, %s, %s, %d, %d, %d, %d, %d);"
            % (int(msg_id), int(level_id), sqltext(fmt), sqltext(r2_key), int(nbytes),
               sqltext(filename), sqltext(name), sqltext(credit), sqltext(showcase),
               ("%.6f" % float(fps)) if fps else "NULL",
               int(inputs or 0), 1 if verified else 0, serve, int(posted or 0), now))
        kept += 1

    # Belt and braces: one statement per line, and every line must be a complete SQL statement.
    # If any hex encoding ever went wrong, this catches it before the file reaches production.
    for ln in lines:
        if ln.startswith("--"):
            continue
        if not sqlite3.complete_statement(ln):
            print("REFUSING TO WRITE: generated a line that is not one complete statement:")
            print("  " + ln[:200])
            sys.exit(1)

    with open(OUT_SQL, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    print("wrote %s (%d rows, serve=%d)" % (OUT_SQL, kept, 1 if approve_backfill else 0))
    print("\nApply with:")
    print('  npx wrangler d1 execute %s --remote -c "%s" --file="%s"' % (D1, WRANGLER_CFG, OUT_SQL))
    if not approve_backfill:
        print("\nRows land with serve=0 and are invisible to the mod until approved:")
        print('  npx wrangler d1 execute %s --remote -c "%s" \\' % (D1, WRANGLER_CFG))
        print('    --command "UPDATE tg_macros SET serve = 1 WHERE serve = 0"')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--upload", action="store_true", help="push blobs to R2")
    ap.add_argument("--sql", action="store_true", help="write the D1 index SQL")
    ap.add_argument("--approve", action="store_true",
                    help="emit rows with serve=1 (use for the reviewed backfill only)")
    ap.add_argument("--workers", type=int, default=6)
    args = ap.parse_args()

    if not (args.upload or args.sql):
        ap.print_help()
        return 1

    c = sqlite3.connect(DB)
    if args.upload:
        upload(c, args.workers)
    if args.sql:
        emit_sql(c, args.approve)
    c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
