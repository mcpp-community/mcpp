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
    text = p.read_text()
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

echo "workflow syntax OK"
