#!/usr/bin/env bash
# requires: unix-shell
# `mcpp test --timeout <secs>`: a hung test is killed and reported as a
# timeout failure; the rest of the suite still runs.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "hang"
version = "0.1.0"
standard = "c++23"
EOF
cat > tests/sleepy.cpp <<'EOF'
import std;
int main() { std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }
EOF
cat > tests/quick.cpp <<'EOF'
int main() { return 0; }
EOF

start=$(date +%s)
set +e
out=$("$MCPP" test --timeout 2 2>&1)
code=$?
set -e
elapsed=$(( $(date +%s) - start ))
[[ $code -eq 1 ]] || { echo "expected exit 1, got $code: $out"; exit 1; }
[[ $elapsed -lt 25 ]] || { echo "timeout did not take effect (took ${elapsed}s)"; exit 1; }
[[ "$out" == *"sleepy ... FAIL (timeout"* ]] || { echo "missing timeout marker: $out"; exit 1; }
[[ "$out" == *"quick ... ok"* ]] || { echo "quick test did not run: $out"; exit 1; }

set +e
outj=$("$MCPP" test --timeout 2 --message-format json 2>/dev/null)
set -e
echo "$outj" | grep -q '"test":"sleepy","status":"run_fail"' || { echo "json: no run_fail"; exit 1; }
echo "$outj" | grep -q '"timed_out":true' || { echo "json: timed_out missing: $outj"; exit 1; }
echo OK
