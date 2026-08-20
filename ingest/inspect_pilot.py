"""Re-open every mirrored blob and re-validate it independently of the run that fetched it.

The point is that the pilot's own "ok" is the ingester grading its own homework. This reads the
bytes back off disk, re-decodes them, and checks the recorded size, level id and input count all
still agree - which is what you actually want to see before committing to 1,860 more.
"""
import os
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mirror_telegram as M  # noqa: E402

c = sqlite3.connect(os.path.join(os.path.dirname(os.path.abspath(__file__)), "work.db"))
rows = c.execute(
    "SELECT format, msg_id, doc_msg_id, level_id, bytes, filename, inputs, verified, local "
    "FROM posts WHERE state='mirrored' ORDER BY format, msg_id"
).fetchall()

print("%-5s %-7s %-7s %-11s %-7s %-7s %-3s %-26s %s"
      % ("fmt", "msg", "doc", "level", "bytes", "inputs", "ver", "filename", "re-check"))
bad = 0
for fmt, msg, doc, lvl, nbytes, fn, inputs, ver, path in rows:
    note = "missing-blob"
    if path and os.path.exists(path):
        data = open(path, "rb").read()
        good, reason, lid, fps, n = M.validate(fmt, data, lvl)
        note = "ok" if good else "REJECT:" + reason
        if len(data) != nbytes:
            note += " SIZE-MISMATCH(%d)" % len(data)
        if fmt == "gdr2" and n != inputs:
            note += " INPUT-DRIFT(%d)" % n
    if not note.startswith("ok"):
        bad += 1
    print("%-5s %-7d %-7d %-11d %-7d %-7s %-3d %-26s %s"
          % (fmt, msg, doc, lvl, nbytes, inputs, ver, (fn or "")[:26], note))

print()
for fmt, k, tot in c.execute(
        "SELECT format, COUNT(*), SUM(bytes) FROM posts WHERE state='mirrored' GROUP BY format"):
    print("  %-5s %-4d %8.1f KB" % (fmt, k, tot / 1024.0))
print("\n  re-check failures: %d" % bad)

skipped = c.execute("SELECT reason, COUNT(*) FROM skipped WHERE at > 0 GROUP BY reason "
                    "ORDER BY 2 DESC LIMIT 6").fetchall()
if skipped:
    print("\n  skip reasons in db:")
    for r, k in skipped:
        print("    %-30s %d" % (r, k))
