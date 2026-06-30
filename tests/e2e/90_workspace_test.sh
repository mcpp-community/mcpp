#!/usr/bin/env bash
# 90_workspace_test.sh — workspace-aware `mcpp test`: `-p <member>`,
# `--workspace`, and bare `mcpp test` at a virtual workspace root. The headline
# fix: discovery is scoped to the selected member, so two members may each have a
# `tests/main.cpp` (same stem) without the old "duplicate test name 'main'" error.
# See .agents/docs/2026-06-30-workspace-test-and-zero-shell-index-design.md.
#
# requires: gcc
set -euo pipefail

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p liba/src liba/tests libb/src libb/tests

cat > mcpp.toml <<'EOF'
[workspace]
members = ["liba", "libb"]
EOF

# Each member is a library (so its own source doesn't collide with tests/main.cpp);
# the test binary is the standalone tests/main.cpp.
for L in liba libb; do
cat > "$L/mcpp.toml" <<EOF
[package]
name    = "$L"
version = "0.1.0"

[targets.$L]
kind = "lib"
EOF
echo "export module $L;" > "$L/src/$L.cppm"
done

# Both members name their test the SAME stem ('main') — this is exactly what used
# to collide at a workspace root. A real (passing) assertion in each.
cat > liba/tests/main.cpp <<'EOF'
int main() { return (1 + 1 == 2) ? 0 : 1; }
EOF
cat > libb/tests/main.cpp <<'EOF'
int main() { return (2 * 2 == 4) ? 0 : 1; }
EOF

# 1. -p selects one member; its test runs, the other does not.
out=$("$MCPP" test -p liba 2>&1) || { echo "$out"; echo "FAIL: test -p liba errored"; exit 1; }
echo "$out" | grep -q "1 passed" || { echo "$out"; echo "FAIL: liba test did not pass"; exit 1; }
echo "$out" | grep -qi "testing member 'libb'" && { echo "FAIL: -p liba also ran libb"; exit 1; }

# 2. --workspace runs BOTH members with no duplicate-stem error.
out=$("$MCPP" test --workspace 2>&1) || { echo "$out"; echo "FAIL: test --workspace errored"; exit 1; }
echo "$out" | grep -qi "duplicate test name" && { echo "$out"; echo "FAIL: duplicate-stem bug not fixed"; exit 1; }
echo "$out" | grep -qi "testing member 'liba'" || { echo "$out"; echo "FAIL: liba not tested"; exit 1; }
echo "$out" | grep -qi "testing member 'libb'" || { echo "$out"; echo "FAIL: libb not tested"; exit 1; }

# 3. Bare `mcpp test` at a virtual workspace root fans out the same way.
out=$("$MCPP" test 2>&1) || { echo "$out"; echo "FAIL: bare test at ws root errored"; exit 1; }
echo "$out" | grep -qi "duplicate test name" && { echo "FAIL: bare test still collides"; exit 1; }
echo "$out" | grep -qi "testing member 'libb'" || { echo "FAIL: bare test did not fan out"; exit 1; }

# 4. A failing member fails the workspace run (non-zero) but the other still runs.
echo 'int main(){return 1;}' > libb/tests/main.cpp
set +e
out=$("$MCPP" test --workspace 2>&1); rc=$?
set -e
[ "$rc" -ne 0 ] || { echo "$out"; echo "FAIL: failing member did not fail the run"; exit 1; }
echo "$out" | grep -qi "member(s) failed: libb" || { echo "$out"; echo "FAIL: no per-member failure summary"; exit 1; }
echo "$out" | grep -qi "testing member 'liba'" || { echo "FAIL: did not continue past failure"; exit 1; }

echo "OK"
