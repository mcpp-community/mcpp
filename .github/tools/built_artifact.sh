#!/usr/bin/env bash
# THE PROGRAM A BUILD MADE, AS THE BUILD REPORTS IT.
#
#   built_artifact.sh <mcpp> <name> [build arguments...]
#
# Runs `<mcpp> build --message-format json [arguments]` and prints the absolute
# path of the artifact named <name> (docs/50-machine-output.md §9: an
# `artifact` event per product, and the `artifacts` of the closing record).
# The build's narration stays on standard error. A failed build, or one that
# reports no such artifact, exits non-zero.
#
# Instead of a search of target/: a self-hosting build keeps the engine it
# runs under target/<triple>/<fingerprint>/engine/epoch-N/mcpp, and a search
# for a file named mcpp took that copy (an x86_64 program) for the product of
# an aarch64 build (2026-11-14).
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: $0 <mcpp> <name> [build arguments...]" >&2; exit 2; }
mcpp=$1; name=$2; shift 2
report=$(mktemp)
trap 'rm -f "$report"' EXIT
"$mcpp" build --message-format json "$@" > "$report"
py=python3; command -v python3 > /dev/null 2>&1 || py=python
"$py" - "$report" "$name" <<'PY'
import json, sys
report, name = sys.argv[1], sys.argv[2]
found = None
with open(report, encoding="utf-8") as f:
    for line in f:
        try:
            doc = json.loads(line)
        except ValueError:
            continue
        if not isinstance(doc, dict):
            continue
        records = []
        if isinstance(doc.get("artifact"), dict):
            records.append(doc["artifact"])
        records.extend(a for a in doc.get("artifacts") or [] if isinstance(a, dict))
        for a in records:
            if a.get("name") == name and a.get("path"):
                found = a["path"]
if not found:
    sys.exit(f"the build reported no artifact named {name}")
print(found)
PY
