#!/usr/bin/env bash
# requires: gcc llvm
# 783_cdb_switches_whole_with_the_configuration.sh — design 2026-09-26
# .agents/docs/2026-09-26-compile-database-and-issue-699-design.md §3.2: one
# database per configuration, identified by the output directory (toolchain,
# target, profile, features). `mcpp test` then `mcpp build` in the SAME
# configuration keep the test entries (merged within one configuration's
# database, item 1). Switching `--toolchain` switches the whole root file
# (item 2: replaced, never merged across configurations); switching back
# restores that configuration's entries, its test units included.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
"$MCPP" new app > /dev/null
cd app

driver_of() {  # $1 = compiler basename fragment ("g++" or "clang++")
    python3 -c "
import json, sys
entries = json.load(open('compile_commands.json'))
for e in entries:
    if 'test_smoke' in e['file'] or 'main.cpp' in e['file']:
        print(e['arguments'][0])
"
}

# `mcpp test` (llvm): a complete database that includes the test file.
"$MCPP" test --toolchain llvm@22.1.8 > test1.log 2>&1 || { cat test1.log; echo "FAIL: mcpp test (llvm) failed"; exit 1; }
grep -q "test_smoke" compile_commands.json || {
    echo "FAIL: after 'mcpp test' (llvm), no entry for tests/test_smoke.cpp"
    cat compile_commands.json; exit 1
}
driver_of | grep -q "clang" || { echo "FAIL: the llvm test entry does not name a clang driver"; exit 1; }

# `mcpp build` in the SAME configuration: the test entry survives (item 1).
"$MCPP" build --toolchain llvm@22.1.8 > build1.log 2>&1 || { cat build1.log; echo "FAIL: mcpp build (llvm) failed"; exit 1; }
grep -q "test_smoke" compile_commands.json || {
    echo "FAIL: 'mcpp build' in the same (llvm) configuration lost the test entry"
    cat compile_commands.json; exit 1
}

# Switch to gcc: the root holds ONLY gcc's configuration -- no llvm entries,
# no test entries (gcc's own configuration was never tested).
"$MCPP" build --toolchain gcc@16.1.0 > build2.log 2>&1 || { cat build2.log; echo "FAIL: mcpp build (gcc) failed"; exit 1; }
grep -q "test_smoke" compile_commands.json && {
    echo "FAIL: llvm's test entries leaked into the gcc configuration"
    cat compile_commands.json; exit 1
}
driver_of | grep -q "clang" && { echo "FAIL: a clang driver survives under the gcc configuration"; exit 1; }
python3 -c "
import json
entries = json.load(open('compile_commands.json'))
main = next(e for e in entries if e['file'].endswith('main.cpp'))
assert 'g++' in main['arguments'][0] or 'gcc' in main['arguments'][0], main['arguments'][0]
"

# Switch back to llvm: that configuration's database, test entry included, is
# restored whole -- it was never touched by the gcc build in between.
"$MCPP" build --toolchain llvm@22.1.8 > build3.log 2>&1 || { cat build3.log; echo "FAIL: mcpp build (llvm again) failed"; exit 1; }
grep -q "test_smoke" compile_commands.json || {
    echo "FAIL: llvm's test entries did not return when switching back"
    cat compile_commands.json; exit 1
}
driver_of | grep -q "clang" || { echo "FAIL: the root is not llvm's configuration after switching back"; exit 1; }

echo "ok: mcpp test then mcpp build keep the test entry in one configuration"
echo "ok: switching toolchain switches the whole root file"
echo "ok: switching back restores that configuration's entries, test units included"
echo "PASS: 783 one compile database per configuration"
