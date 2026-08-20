#!/usr/bin/env bash
# Publish a CI-built .geode to the site, then print the URL for the Geode index.
#
#   ./release.sh path/to/jackz.click-indicators.geode
#
# The index rejects locally compiled mods, so this takes the artifact downloaded from
# the GitHub Actions run rather than reading build/ - and checks it before publishing.
set -e
cd "$(dirname "$0")"

GEODE="${1:-}"
if [ -z "$GEODE" ]; then
    echo "usage: ./release.sh <path-to-.geode-or-artifact-zip-from-CI>" >&2
    echo >&2
    echo "Download the 'Click Indicators' artifact from the Actions run page." >&2
    exit 2
fi
[ -f "$GEODE" ] || { echo "no such file: $GEODE" >&2; exit 2; }

# The licence gate depends on src/cicrypt.cpp. A verifier that has not been checked against the
# published vectors is worse than no verifier at all: one that accepts everything hands the product
# away silently, and one that rejects everything signs out every paying customer at once. So the
# vectors run before anything is published, and a failure stops the release rather than reaching
# 1,222 installs.
if [ -f tools/crypttest.bat ]; then
    echo "checking the licence crypto against RFC 8032 / FIPS 180-4 and the server's own signer..."
    if ! cmd.exe /c "tools\crypttest.bat" >/tmp/crypttest.log 2>&1; then
        tail -25 /tmp/crypttest.log >&2
        echo >&2
        echo "licence crypto FAILED - refusing to publish." >&2
        exit 3
    fi
    tail -1 /tmp/crypttest.log
fi

# Read AFTER the artifact is unwrapped, from the artifact itself - see below. Reading it from
# the repo's mod.json meant that publishing an older CI build archived it under the repo's
# version and printed "expect vX" for a version the site would never report, defeating the
# post-deploy check that exists to catch exactly that mixup.
VER=""

# GitHub always wraps artifacts in a zip, so the download is "Click Indicators.zip" with the
# .geode inside. Unwrap it - publishing the wrapper would ship a file nobody can install, and
# the local-path check below would pass for the wrong reason, since the inner file is
# compressed and none of its strings are greppable.
UNWRAPPED=""
if python - "$GEODE" <<'PY'
import sys, zipfile
try:
    with zipfile.ZipFile(sys.argv[1]) as z:
        names = z.namelist()
except Exception:
    sys.exit(1)
# a .geode is itself a zip, so tell them apart by what is inside
sys.exit(0 if any(n.lower().endswith(".geode") for n in names) else 1)
PY
then
    TMP=$(mktemp -d)
    UNWRAPPED=$(python - "$GEODE" "$TMP" <<'PY'
import sys, zipfile, os
with zipfile.ZipFile(sys.argv[1]) as z:
    inner = [n for n in z.namelist() if n.lower().endswith(".geode")]
    if len(inner) != 1:
        print("EXPECTED EXACTLY ONE .geode, FOUND %d" % len(inner), file=sys.stderr)
        sys.exit(1)
    z.extract(inner[0], sys.argv[2])
    print(os.path.join(sys.argv[2], inner[0]))
PY
    ) || exit 1
    echo "unwrapped GitHub artifact -> $(basename "$UNWRAPPED")"
    GEODE="$UNWRAPPED"
fi

# A local build bakes the developer's home directory into the binary through a dependency
# that puts __FILE__ in its own sources. That is how the first submission was spotted as
# locally compiled, so refuse to publish anything still carrying those strings.
python - "$GEODE" <<'PY'
import sys, zipfile, re
# CI runners have home directories too - /Users/runner on macOS, /home/runner on Linux - so the
# test is not "does a home path appear" but "is it somebody's home rather than the runner's".
# The original blanket check was written when only Windows was built; it refused every
# legitimate mac and android artifact the moment those targets were added.
PATTERNS = [
    re.compile(br"C:[/\\]Users[/\\]"),                    # any Windows home; CI never builds there
    re.compile(br"/Users/(?!runner/)[A-Za-z0-9._-]+/"),   # a real person's mac
    re.compile(br"/home/(?!runner/)[A-Za-z0-9._-]+/"),    # a real person's linux box
]
hits = set()
with zipfile.ZipFile(sys.argv[1]) as z:
    for entry in z.namelist():
        blob = z.read(entry)
        for rx in PATTERNS:
            m = rx.search(blob)
            if m:
                hits.add("%s (%s)" % (entry, m.group(0).decode("utf-8", "replace")))
if hits:
    print("REFUSING TO PUBLISH - local build paths found in: " + ", ".join(sorted(hits)),
          file=sys.stderr)
    print("This looks like a local build. Publish the artifact from GitHub Actions instead.",
          file=sys.stderr)
    sys.exit(1)
print("checked: no local build paths in the artifact")
PY

# The .geode is bundled into the API Worker rather than dropped in the site's public
# folder, so /api/download can require a paid session before handing it over. This also
# regenerates the release notes the dashboard shows, straight from changelog.md.
python server/gen-release.py "$GEODE"

# Keep a copy of everything published, outside the web root.
# The version being published is the artifact's own, never the repo's.
VER=$(python -c "
import json,sys,zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    n=[x for x in z.namelist() if x.lower()=='mod.json']
    print(json.loads(z.read(n[0]).decode('utf-8'))['version'])
" "$GEODE")
echo "publishing $VER"

mkdir -p archive/published-builds
cp "$GEODE" "archive/published-builds/clickindicators-$VER.geode"

CI=true wrangler deploy -c server/wrangler.jsonc
CI=true wrangler deploy -c wrangler.site.jsonc

echo
echo "Live. Buyers get it from https://clickindicatorsmod.com/account.html"
echo
echo "Check the gate is closed before telling anyone:"
echo "  curl -si https://clickindicatorsmod.com/api/download | head -1     # expect 401"
echo "  curl -s  https://clickindicatorsmod.com/api/release | head -c 200  # expect $VER"
