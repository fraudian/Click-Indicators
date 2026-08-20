"""Re-scrape the posts rejected as pair-unverified and show the caption/filename pair, so the
check can be judged on evidence rather than assumed correct.

A rejection is RIGHT if the caption plainly describes a different level from the file. It is WRONG
if the two obviously refer to the same level and only differ cosmetically - and every wrong one is
a level the mod could have supported and does not.
"""
import re
import sqlite3
import sys
import time
import urllib.request
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from index_pass import (BUBBLE_RE, DOC_RE, TEXT_RE, TEXT_FALLBACK_RE, UA,  # noqa: E402
                        inner_text, parse_caption, PREFER, SIZE_CAP)

HERE = os.path.dirname(os.path.abspath(__file__))
c = sqlite3.connect(os.path.join(HERE, "work.db"))
ids = [r[0] for r in c.execute(
    "SELECT msg_id FROM skipped WHERE reason='pair-unverified' ORDER BY msg_id DESC")]

print("%d posts rejected as pair-unverified; sampling %d\n" % (len(ids), min(len(ids), 14)))
print("%-7s %-30s %-30s %s" % ("msg", "caption name", "file basename", "verdict"))

shown = 0
for target in ids:
    if shown >= 14:
        break
    url = "https://t.me/s/gdmacros?before=%d" % (target + 12)
    try:
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        html = urllib.request.urlopen(req, timeout=40).read().decode("utf-8", "replace")
    except Exception:
        continue

    caps, docs = {}, {}
    for mid_s, frag in BUBBLE_RE.findall(html):
        mid = int(mid_s)
        tm = TEXT_RE.search(frag) or TEXT_FALLBACK_RE.search(frag)
        if tm and tm.group(1).strip():
            caps[mid] = tm.group(1)
        d = DOC_RE.findall(frag)
        if d:
            docs[mid] = d
    if target not in docs:
        continue

    cap_html = ""
    for back in (1, 2, 3, 4, 5):
        if target - back in caps:
            cap_html = caps[target - back]
            break
    info = parse_caption(cap_html) if cap_html else None
    if not info:
        continue

    files = {}
    for did, title, extra in docs[target]:
        fn = inner_text(title)
        ext = fn.rsplit(".", 1)[-1].lower() if "." in fn else ""
        files[ext] = fn
    picked = next((files[f] for f in PREFER if f in files), None)
    if not picked:
        continue

    base = re.sub(r"\s*\(\d+\)$", "", picked.rsplit(".", 1)[0]).strip()
    nm = info["name"].strip()
    same_start = base[:6].lower() == nm[:6].lower()
    verdict = "LOOKS LIKE THE SAME LEVEL" if same_start else "genuinely different"
    print("%-7d %-30s %-30s %s" % (target, nm[:30], base[:30], verdict))
    shown += 1
    time.sleep(0.6)
