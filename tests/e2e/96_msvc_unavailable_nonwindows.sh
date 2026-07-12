#!/usr/bin/env bash
# requires: unix-shell
# 96_msvc_unavailable_nonwindows.sh — msvc@system is Windows-only:
#   - `toolchain default/install msvc` fail with a host error (no network,
#     no sandbox mutation)
#   - `toolchain list` has no System section off-Windows
#   - a manifest pinned to msvc@system fails the same way at build time
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

rc=0; out=$("$MCPP" toolchain default msvc 2>&1) || rc=$?
[[ $rc -ne 0 && "$out" == *"only available on Windows"* ]] \
    || { echo "FAIL: default msvc: rc=$rc out=$out"; exit 1; }

rc=0; out=$("$MCPP" toolchain install msvc 2>&1) || rc=$?
[[ $rc -ne 0 && "$out" == *"only available on Windows"* ]] \
    || { echo "FAIL: install msvc: rc=$rc out=$out"; exit 1; }

out=$("$MCPP" toolchain list 2>&1) || true
[[ "$out" != *"System:"* ]] || { echo "FAIL: System section off-Windows: $out"; exit 1; }

# Manifest pinned to msvc@system errors out at resolution, not mid-build.
mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
cat > mcpp.toml <<EOF
[package]
name    = "msvcpin"
version = "0.1.0"

[toolchain]
default = "msvc@system"
EOF
cat > src/main.cpp <<'EOF'
import std;
int main() { std::println("never built"); return 0; }
EOF
rc=0; out=$("$MCPP" build 2>&1) || rc=$?
[[ $rc -ne 0 && "$out" == *"only available on Windows"* ]] \
    || { echo "FAIL: build with msvc@system manifest: rc=$rc out=$out"; exit 1; }

echo "PASS: msvc@system correctly rejected off-Windows"
