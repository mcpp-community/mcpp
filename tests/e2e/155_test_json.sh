#!/usr/bin/env bash
# requires:
# --message-format json: NDJSON per test, package error record, pure stdout
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "jsonout"
version = "0.1.0"
standard = "c++23"
EOF
cat > tests/ok.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("hello from ok"); return 0; }
EOF
echo 'int main() { return 1; }' > tests/runfail.cpp
echo 'int main() { D2X_YOUR_ANSWER x = 1; return x; }' > tests/nocompile.cpp

set +e
out=$("$MCPP" test --message-format json 2>/dev/null)
code=$?
set -e
[[ $code -eq 1 ]] || { echo "expected exit 1, got $code"; exit 1; }

# stdout must be pure NDJSON: every line parses as a JSON object.
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    [[ "$line" == "{"* ]] || { echo "non-JSON line on stdout: $line"; exit 1; }
done <<< "$out"

echo "$out" | grep -q '"test":"ok","status":"pass"'            || { echo "missing pass record"; exit 1; }
echo "$out" | grep -q '"test":"runfail","status":"run_fail"'    || { echo "missing run_fail record"; exit 1; }
echo "$out" | grep -q '"exit_code":1'                            || { echo "missing exit_code"; exit 1; }
echo "$out" | grep -q '"test":"nocompile","status":"compile_fail"' || { echo "missing compile_fail record"; exit 1; }
echo "$out" | grep -q 'D2X_YOUR_ANSWER'                          || { echo "compile_output missing diagnostics"; exit 1; }
echo "$out" | grep -q 'hello from ok'                            || { echo "run_output not captured"; exit 1; }
echo "$out" | grep -q '"summary":{"passed":1,"failed":2'         || { echo "missing summary"; exit 1; }

# Invalid format value → usage error
set +e
"$MCPP" test --message-format yaml >/dev/null 2>&1
[[ $? -eq 2 ]] || { echo "invalid format should exit 2"; exit 1; }
set -e
echo OK
