#!/usr/bin/env bash
# requires: llvm python3
# 1025 -- THE DEFAULT PLATFORM IS ONE RELEASE (design 2026-11-07 §4.10, D20).
# The target table names the openkal platform release this mcpp was qualified
# with, so a project that states none builds the same versions at any time,
# with or without mcpp.lock; a project selects another release in its manifest.
#
# Criteria:
#   A. A project that states no platform builds `x86_64-openkal` over
#      openkal-platform 0.24.3, from the target table, and mcpp.lock records it.
#   B. A lock that records another release of it does not decide: the build
#      says which it takes and which the lock recorded, and records 0.24.3.
#   C. A platform the project states is taken as it is (a series here).
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3

cd "$TMP"
"$MCPP" new app > /dev/null 2>&1
cd app
locked() { $PY - <<'PY'
import re
text = open("mcpp.lock").read()
m = re.search(r'\[package\."openkal-platform"\][^\[]*?version\s*=\s*"([^"]+)"', text)
print(m.group(1) if m else "")
PY
}

# A
"$MCPP" build --target x86_64-openkal > a.log 2>&1 || fail "A: the openkal build failed" a.log
grep -q 'Platform *openkal-platform 0.24.3' a.log || fail "A: the build does not use openkal-platform 0.24.3" a.log
[[ "$(locked)" == "0.24.3" ]] || fail "A: mcpp.lock does not record 0.24.3" mcpp.lock

# B
$PY - <<'PY'
import re
text = open("mcpp.lock").read()
text = re.sub(r'(\[package\."openkal-platform"\][^\[]*?version\s*=\s*")0\.24\.3(")', r'\g<1>0.24.2\2', text, count=1)
open("mcpp.lock", "w").write(text)
PY
[[ "$(locked)" == "0.24.2" ]] || fail "B: the fixture could not edit mcpp.lock" mcpp.lock
"$MCPP" build --target x86_64-openkal > b.log 2>&1 || fail "B: the openkal build failed" b.log
grep -q "default; mcpp.lock recorded v0.24.2" b.log || fail "B: the build does not say which release it takes over the lock's" b.log
[[ "$(locked)" == "0.24.3" ]] || fail "B: mcpp.lock was not rewritten to 0.24.3" mcpp.lock

# C
cat >> mcpp.toml <<'EOT'

[target.'cfg(os = "openkal")']
platform = "openkal-platform@^0.24"
EOT
"$MCPP" build --target x86_64-openkal > c.log 2>&1 || fail "C: the build with a stated series failed" c.log
grep -q 'Platform *openkal-platform 0.24' c.log || fail "C: no platform line" c.log
grep -q "'s default" c.log && fail "C: a stated platform was reported as the default" c.log

echo "PASS: 1025 the default platform is one release"
