#!/usr/bin/env bash
# requires: python3
# Every .github/workflows/*.yml must be loadable YAML.
#
# This exists because `bench.yml` was committed with
#
#     run: "$BENCH" --list
#
# which YAML reads as a quoted scalar followed by garbage. The file parsed
# nowhere, so the workflow could never start — and NOTHING SAID SO. GitHub still
# lists a broken workflow as "active", a `workflow_dispatch`-only workflow is
# never exercised by a push, and no test looked at it. It was invisible until
# someone tried to load the file by hand.
#
# The check is deliberately syntax-only. Validating the schema would need the
# full Actions grammar; the failure mode that actually happened is a file that
# does not parse, and that costs a few lines to rule out forever.
set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$REPO/.github/workflows"
[ -d "$DIR" ] || { echo "no workflows directory at $DIR"; exit 1; }

count=$(find "$DIR" -maxdepth 1 \( -name '*.yml' -o -name '*.yaml' \) | wc -l)
[ "$count" -gt 0 ] || { echo "no workflow files found under $DIR"; exit 1; }

python3 - "$DIR" <<'PYEOF'
import pathlib, re, sys

# PyYAML is not everywhere — the macOS runner has none, and a test that
# hard-fails on a missing dev dependency is a test that gets deleted. So: full
# parse where it exists, targeted lint where it does not, and SAY WHICH RAN. A
# fallback that is quietly weaker than the check it replaces is how a green
# stops meaning anything.
try:
    import yaml
    HAVE_YAML = True
except ImportError:
    HAVE_YAML = False

# The exact failure this test exists for: a scalar that opens with a quote and
# carries more content after the closing one —
#     run: "$BENCH" --list
# YAML reads that as a quoted scalar followed by garbage and refuses the file.
TRAILING_AFTER_QUOTED = re.compile(r'^\s*[\w.-]+:\s*"[^"]*"\s*\S')

bad = []
files = sorted(p for p in pathlib.Path(sys.argv[1]).iterdir()
               if p.suffix in (".yml", ".yaml"))
for p in files:
    # encoding is NOT optional: Python on Windows defaults to the ANSI code
    # page, and these files are UTF-8 (em dashes in the comments are enough).
    # Without it the check dies with
    #     UnicodeDecodeError: 'charmap' codec can't decode byte 0x8d
    # on the runner and nowhere else.
    text = p.read_text(encoding="utf-8")
    for n, line in enumerate(text.splitlines(), 1):
        if TRAILING_AFTER_QUOTED.match(line):
            bad.append(f"{p.name}:{n}: content after a quoted scalar: {line.strip()}")
    if not HAVE_YAML:
        continue
    try:
        doc = yaml.safe_load(text)
    except Exception as e:
        bad.append(f"{p.name}: {e}")
        continue
    # A workflow with no `jobs` parses but can never do anything — the same class
    # of silent nothing, so it is reported the same way.
    if not isinstance(doc, dict) or not doc.get("jobs"):
        bad.append(f"{p.name}: parsed, but declares no jobs")

if bad:
    print("malformed workflow files:")
    for b in bad:
        print("  " + b)
    sys.exit(1)

mode = "parsed" if HAVE_YAML else "linted (no PyYAML here — quoted-scalar check only)"
print(f"{len(files)} workflow files {mode}")
PYEOF

# ── every release-archive download goes through the retrying fetcher ────────
#
# A bare `curl -fsSL -o <archive>` was the single largest source of unexplained
# CI red on this repository:
#
#     curl: (52) Empty reply from server
#     Error: Process completed with exit code 52
#
# — the release CDN accepting the connection and closing it with no response.
# It is transient, it hits Windows hardest, and the log carries no test name, so
# it reads like a code failure every time.
#
# Two things make it come back, and this guard catches both:
#   * a NEW download added with a plain curl, because the surrounding lines all
#     look like that;
#   * someone "fixing" it with `--retry` alone, which does NOT cover exit 52 —
#     an empty reply is a transport error, not one of the HTTP statuses
#     `--retry` knows about.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -x "$ROOT/.github/tools/fetch_release.sh" ]   || { echo "FAIL: .github/tools/fetch_release.sh is missing or not executable"; exit 1; }

bare=$(grep -rn -- '-o "\${WORK}/\|-o "/tmp/' "$ROOT/.github/workflows" "$ROOT/.github/actions" 2>/dev/null        | grep 'curl' | grep -v 'retry-all-errors' || true)
if [ -n "$bare" ]; then
    echo "FAIL: an archive is downloaded with a bare curl; use .github/tools/fetch_release.sh"
    echo "      (a plain curl here is the 'curl: (52) Empty reply from server' flake,"
    echo "       and --retry alone does not cover it)"
    echo "$bare"
    exit 1
fi
echo "release archive downloads: all via fetch_release.sh"

echo "workflow syntax OK"
