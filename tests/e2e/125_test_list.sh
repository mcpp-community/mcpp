#!/usr/bin/env bash
# requires:
# `mcpp test --list`: enumerate (filtered) tests without building anything.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests/00-a pkg/tests/01-b
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "listing"
version = "0.1.0"
standard = "c++23"
EOF
echo 'int main() { return 0; }' > tests/00-a/0.cpp
echo 'this does not even parse' > tests/01-b/0.cpp   # --list 不构建,坏文件也要列出

out=$("$MCPP" test --list 2>&1) || { echo "--list failed: $out"; exit 1; }
[[ "$out" == *"00-a/0"* && "$out" == *"01-b/0"* ]] || { echo "missing names: $out"; exit 1; }
[[ ! -d target ]] || { echo "--list must not build (target/ created)"; exit 1; }

out=$("$MCPP" test --list 00-a --message-format json 2>/dev/null)
echo "$out" | grep -q '"test":"00-a/0","main":".*tests/00-a/0.cpp"' \
    || { echo "json list record missing/bad: $out"; exit 1; }
echo "$out" | grep -qv '"test":"01-b/0"' || { echo "filter leaked into list"; exit 1; }
echo "$out" | grep -q '"summary":{"total":1}' || { echo "missing list summary: $out"; exit 1; }
echo OK
