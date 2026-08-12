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
# does not parse, and that costs three lines to rule out forever.
set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$REPO/.github/workflows"
[ -d "$DIR" ] || { echo "no workflows directory at $DIR"; exit 1; }

count=$(find "$DIR" -maxdepth 1 -name '*.yml' -o -maxdepth 1 -name '*.yaml' | wc -l)
[ "$count" -gt 0 ] || { echo "no workflow files found under $DIR"; exit 1; }

python3 - "$DIR" <<'PY'
import pathlib, sys, yaml

bad = []
files = sorted(p for p in pathlib.Path(sys.argv[1]).iterdir()
               if p.suffix in (".yml", ".yaml"))
for p in files:
    try:
        doc = yaml.safe_load(p.read_text())
    except Exception as e:
        bad.append(f"{p.name}: {e}")
        continue
    # A workflow with no `jobs` parses but can never do anything — the same
    # class of silent nothing, so it is reported the same way.
    if not isinstance(doc, dict) or not doc.get("jobs"):
        bad.append(f"{p.name}: parsed, but declares no jobs")

if bad:
    print("malformed workflow files:")
    for b in bad:
        print("  " + b)
    sys.exit(1)
print(f"{len(files)} workflow files parse and declare jobs")
PY

echo "workflow syntax OK"
