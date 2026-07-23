#!/usr/bin/env bash
# requires:
# tests/ subdirectories: same stem in two dirs must coexist, names are relative paths
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests/00-a pkg/tests/01-b
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "subdirs"
version = "0.1.0"
standard = "c++23"
EOF
cat > tests/00-a/0.cpp <<'EOF'
int main() { return 0; }
EOF
cat > tests/01-b/0.cpp <<'EOF'
int main() { return 0; }
EOF

out=$("$MCPP" test 2>&1) || { echo "mcpp test failed: $out"; exit 1; }
[[ "$out" == *"00-a/0 ... ok"* ]] || { echo "missing path-based name 00-a/0: $out"; exit 1; }
[[ "$out" == *"01-b/0 ... ok"* ]] || { echo "missing path-based name 01-b/0: $out"; exit 1; }
[[ "$out" == *"2 passed"* ]]     || { echo "expected 2 passed: $out"; exit 1; }
echo OK
