#!/usr/bin/env bash
# requires:
# `mcpp test <pattern>`: substring filter on path-based test names
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests/00-a pkg/tests/01-b
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "filter"
version = "0.1.0"
standard = "c++23"
EOF
echo 'int main() { return 0; }' > tests/00-a/0.cpp
echo 'int main() { return 0; }' > tests/01-b/0.cpp

out=$("$MCPP" test 00-a 2>&1) || { echo "filtered run failed: $out"; exit 1; }
[[ "$out" == *"00-a/0 ... ok"* ]] || { echo "filtered test did not run: $out"; exit 1; }
[[ "$out" != *"01-b/0"* ]]        || { echo "filter leaked other tests: $out"; exit 1; }
[[ "$out" == *"1 passed"* ]]      || { echo "bad summary: $out"; exit 1; }

set +e
out2=$("$MCPP" test does-not-exist 2>&1)
code2=$?
set -e
[[ $code2 -eq 2 ]] || { echo "no-match should exit 2, got $code2: $out2"; exit 1; }
[[ "$out2" == *"no tests match"* ]] || { echo "missing no-match error: $out2"; exit 1; }
echo OK
