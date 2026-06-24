#!/usr/bin/env bash
# requires:
# 77_cdb_preserves_test_entries.sh — a plain `mcpp build` must NOT wipe the
# tests/ entries that a prior `mcpp test` wrote into compile_commands.json.
#
# Regression for the "no completion in tests/" bug: `mcpp build` regenerates the
# CDB from a plan that excludes test files + dev-deps, so before the merge fix it
# overwrote the complete CDB `mcpp test` produced, and clangd lost all coverage
# of tests/*.cpp (gtest, import std, ...). The fix preserves still-valid prior
# entries. `mcpp build` itself still does ZERO dev-dep resolution — build-only
# users are unaffected; coverage simply survives across the edit→build loop.
#
# No `requires:` capability → runs on all three CI platforms, mirroring the
# other `mcpp test` e2e tests (15/16/17).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new app > /dev/null
cd app

cdb=compile_commands.json
test_file='test_smoke\.cpp'

# 1. `mcpp test` produces a complete CDB that includes the test file.
"$MCPP" test > /dev/null
grep -q "$test_file" "$cdb" || {
    echo "FAIL: after 'mcpp test', $cdb has no entry for tests/test_smoke.cpp"
    cat "$cdb"; exit 1
}

# 2. Force a real `mcpp build` rebuild (defeat the build.ninja fast-path) so the
#    CDB is actually regenerated — otherwise the merge path isn't exercised.
printf '\n// touch to force rebuild\n' >> src/main.cpp
"$MCPP" build > /dev/null

# 3. The test entry must survive, and the regular source must still be present.
grep -q "$test_file" "$cdb" || {
    echo "FAIL: 'mcpp build' wiped the tests/test_smoke.cpp entry from $cdb"
    cat "$cdb"; exit 1
}
grep -q 'main\.cpp' "$cdb" || {
    echo "FAIL: $cdb lost the src/main.cpp entry after build"
    cat "$cdb"; exit 1
}

# 4. Deleting a test file must prune its stale entry on the next build.
rm tests/test_smoke.cpp
printf '\n// touch again\n' >> src/main.cpp
"$MCPP" build > /dev/null
if grep -q "$test_file" "$cdb"; then
    echo "FAIL: $cdb kept a stale entry for the deleted tests/test_smoke.cpp"
    cat "$cdb"; exit 1
fi

echo "OK"
