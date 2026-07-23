#!/usr/bin/env bash
# requires:
# Per-test compile isolation: a non-compiling test fails alone; others build & run.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "isolation"
version = "0.1.0"
standard = "c++23"
EOF
cat > tests/ok.cpp <<'EOF'
int main() { return 0; }
EOF
cat > tests/runfail.cpp <<'EOF'
int main() { return 1; }
EOF
cat > tests/nocompile.cpp <<'EOF'
int main() { D2X_YOUR_ANSWER x = 1; return x; }
EOF

set +e
out=$("$MCPP" test 2>&1)
code=$?
set -e
[[ $code -eq 1 ]] || { echo "expected exit 1, got $code: $out"; exit 1; }
[[ "$out" == *"ok ... ok"* ]]                 || { echo "passing test did not run: $out"; exit 1; }
[[ "$out" == *"runfail ... FAIL (exit 1)"* ]] || { echo "runtime failure not reported: $out"; exit 1; }
[[ "$out" == *"nocompile ... FAIL (compile)"* ]] || { echo "compile failure not isolated: $out"; exit 1; }
[[ "$out" == *"1 passed"* && "$out" == *"2 failed"* ]] || { echo "bad summary: $out"; exit 1; }

# Package-level failure stays a hard error: break a src module, all-red is wrong,
# 'build failed' is right.
mkdir -p src
cat > src/isolation.cppm <<'EOF'
export module isolation;
this is not C++;
EOF
set +e
out2=$("$MCPP" test 2>&1)
code2=$?
set -e
[[ $code2 -ne 0 ]] || { echo "package error must fail: $out2"; exit 1; }
[[ "$out2" != *"nocompile ... FAIL (compile)"* ]] || { echo "package error misattributed to tests: $out2"; exit 1; }
echo OK
