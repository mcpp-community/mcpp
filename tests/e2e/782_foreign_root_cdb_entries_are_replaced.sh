#!/usr/bin/env bash
# requires:
# 782_foreign_root_cdb_entries_are_replaced.sh — design 2026-09-26
# .agents/docs/2026-09-26-compile-database-and-issue-699-design.md §3.2 items
# 2-3: the root compile_commands.json is a COPY of the current configuration's
# database, replaced whole and never merged with another writer's entries.
# When the replaced file held entries mcpp did not write, one warning states
# how many.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
"$MCPP" new app > /dev/null
cd app
# Pinned rather than left to the machine's default: the property under test
# does not depend on which toolchain is used, and pinning keeps the test's
# reading independent of shared state outside this tree.
TCFLAG=(--toolchain gcc@16.1.0)
"$MCPP" build "${TCFLAG[@]}" > /dev/null 2>&1

# Another writer's database: two entries with no `output` field at all -- the
# shape a hand-written or xmake-style database uses, and never mcpp's own
# (an entry without `output` is not mcpp's, §3.2 item 3).
cat > compile_commands.json <<'EOF'
[
  {"directory": "/somewhere", "file": "/somewhere/other.cpp",
   "arguments": ["cc", "-c", "/somewhere/other.cpp", "-o", "/somewhere/other.o"]},
  {"directory": "/somewhere", "file": "/somewhere/second.cpp",
   "arguments": ["cc", "-c", "/somewhere/second.cpp", "-o", "/somewhere/second.o"]}
]
EOF

# Force a real prepare pass (not the fast path), so the replace actually runs.
touch src/main.cpp
out=$("$MCPP" build "${TCFLAG[@]}" 2>&1) || { echo "$out"; echo "FAIL: build failed"; exit 1; }

warn_count=$(printf '%s\n' "$out" | grep -c 'held 2 entries mcpp did not write')
[[ "$warn_count" -eq 1 ]] || {
    echo "FAIL: expected exactly one warning naming 2 foreign entries, got $warn_count"
    echo "$out"
    exit 1
}
printf '%s\n' "$out" | grep -q "mcpp's configuration" || {
    echo "FAIL: the warning does not say the file now holds mcpp's configuration"
    echo "$out"
    exit 1
}

grep -q "other.cpp" compile_commands.json && {
    echo "FAIL: a foreign entry survived the replace"
    cat compile_commands.json
    exit 1
}
grep -q "second.cpp" compile_commands.json && {
    echo "FAIL: a foreign entry survived the replace"
    cat compile_commands.json
    exit 1
}
grep -q "main.cpp" compile_commands.json || {
    echo "FAIL: the project's own entry is missing after the replace"
    cat compile_commands.json
    exit 1
}

# A second build over the now-mcpp-owned file warns no further: nothing
# foreign is left to replace.
out2=$("$MCPP" build "${TCFLAG[@]}" --no-cache 2>&1) || { echo "$out2"; echo "FAIL: second build failed"; exit 1; }
if printf '%s\n' "$out2" | grep -q "did not write"; then
    echo "FAIL: warned again on a file that already held only mcpp's configuration"
    echo "$out2"
    exit 1
fi

echo "OK"
