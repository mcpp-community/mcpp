#!/usr/bin/env bash
# 1 ok + 1 fail → mcpp test exits 1, summary lists failures.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp

# Replace bundled smoke with one that fails.
cat > tests/test_fail.cpp <<'EOF'
int main() { return 1; }
EOF

# Run; expect non-zero exit
rc=0
out=$("$MCPP" test 2>&1) || rc=$?

[[ $rc -ne 0 ]] || { echo "expected non-zero exit, got 0"; exit 1; }
echo "$out" | grep -q 'test_fail ... FAIL' || { echo "missing FAIL marker: $out"; exit 1; }
echo "$out" | grep -q 'test_smoke ... ok'   || { echo "smoke should still pass: $out"; exit 1; }
echo "$out" | grep -q 'failures:'           || { echo "missing failures list: $out"; exit 1; }
echo "$out" | grep -q '1 passed; 1 failed'  || { echo "missing summary: $out"; exit 1; }

echo "OK"
