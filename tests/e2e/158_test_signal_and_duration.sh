#!/usr/bin/env bash
# requires: unix-shell
# Signaled tests report shell-convention exit codes (128+sig) with the signal
# number in JSON, and every JSON test record carries duration_ms.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "sigdur"
version = "0.1.0"
standard = "c++23"
EOF
cat > tests/crash.cpp <<'EOF'
int main() { int* p = nullptr; return *p; }
EOF
cat > tests/ok.cpp <<'EOF'
int main() { return 0; }
EOF

set +e
out=$("$MCPP" test --message-format json 2>/dev/null)
code=$?
set -e
[[ $code -eq 1 ]] || { echo "expected exit 1, got $code"; exit 1; }

echo "$out" | grep -q '"test":"crash","status":"run_fail","exit_code":139,"signal":11' \
    || { echo "signaled test not normalized to 139/11: $out"; exit 1; }
echo "$out" | grep -q '"test":"ok","status":"pass"' || { echo "ok test missing"; exit 1; }
[[ $(echo "$out" | grep -c '"duration_ms":') -eq 2 ]] \
    || { echo "duration_ms missing from test records: $out"; exit 1; }

# 人读输出同样用 shell 惯例退出码
out2=$("$MCPP" test crash 2>&1) || true
[[ "$out2" == *"crash ... FAIL (exit 139)"* ]] || { echo "human output not 139: $out2"; exit 1; }
echo OK
