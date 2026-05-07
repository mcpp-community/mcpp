#!/usr/bin/env bash
# `mcpp test` discovers tests/**/*.cpp and runs each as a separate binary.
# All passing → exit 0 + summary "ok. N passed".
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp

# mcpp new auto-creates tests/test_smoke.cpp; add a second test for diversity.
cat > tests/test_other.cpp <<'EOF'
import std;
int main() { std::println("test_other: ok"); return 0; }
EOF

out=$("$MCPP" test 2>&1)
echo "$out" | grep -q 'test_smoke ... ok' || { echo "missing smoke pass: $out"; exit 1; }
echo "$out" | grep -q 'test_other ... ok' || { echo "missing other pass: $out"; exit 1; }
echo "$out" | grep -q '2 passed; 0 failed' || { echo "missing summary: $out"; exit 1; }

echo "OK"
