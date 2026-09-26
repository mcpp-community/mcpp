#!/usr/bin/env bash
# requires:
# 781_deleted_root_cdb_is_restored.sh — C1 (design 2026-09-26
# .agents/docs/2026-09-26-compile-database-and-issue-699-design.md §3.2 item
# 4): a deleted root compile_commands.json is restored by the very next
# `mcpp build`, through the FAST PATH -- no plan at all, since nothing on this
# tree changed. Before the fix (2026.9.26.1 and earlier), the fast path never
# reached a compile-database writer, so a deleted database stayed deleted
# forever (#397 C-1, #677 B1, report §3.6).
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

"$MCPP" build "${TCFLAG[@]}" > build1.log 2>&1 || { cat build1.log; echo "FAIL: the first build failed"; exit 1; }
[[ -s compile_commands.json ]] || { echo "FAIL: no compile_commands.json after the first build"; exit 1; }

config_cdb=$(find target -name compile_commands.json | head -1)
[[ -n "$config_cdb" ]] || { echo "FAIL: no configuration database under target/"; exit 1; }
before=$(cat "$config_cdb")

rm compile_commands.json

"$MCPP" build "${TCFLAG[@]}" > build2.log 2>&1 || {
    cat build2.log
    echo "FAIL: the build after deleting the root database failed"
    exit 1
}

[[ -f compile_commands.json ]] || {
    echo "FAIL: compile_commands.json was not restored"
    cat build2.log
    exit 1
}

# "Without a plan": nothing in the tree changed since the first build, so a
# real prepare pass has nothing to compile -- the restore must not trigger
# one. A plan prints a "Compiling <pkg>" line; the fast path prints only
# "Finished ... in <time>".
if grep -q "Compiling" build2.log; then
    echo "FAIL: restoring the root database ran a full plan"
    cat build2.log
    exit 1
fi

after=$(cat compile_commands.json)
[[ "$after" == "$before" ]] || {
    echo "FAIL: the restored root file differs from the configuration's database"
    diff <(echo "$before") <(echo "$after") || true
    exit 1
}

# The restore survives a SECOND round-trip too (not a one-shot fluke of the
# cache entry the first build happened to leave behind).
rm compile_commands.json
"$MCPP" build "${TCFLAG[@]}" > build3.log 2>&1 || { cat build3.log; echo "FAIL: the third build failed"; exit 1; }
[[ -f compile_commands.json ]] || { echo "FAIL: the second delete was not restored"; exit 1; }

echo "OK"
