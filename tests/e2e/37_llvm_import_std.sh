#!/usr/bin/env bash
# 37_llvm_import_std.sh — build an import-std package with xlings LLVM/libc++.
set -e

LLVM_ROOT="${HOME}/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7"
if [[ ! -x "$LLVM_ROOT/bin/clang++" ]]; then
    echo "SKIP: xlings llvm@20.1.7 is not installed"
    exit 0
fi
if [[ ! -f "$LLVM_ROOT/share/libc++/v1/std.cppm" ]]; then
    echo "SKIP: xlings llvm@20.1.7 has no libc++ std.cppm"
    exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<'EOF'
[package]
name    = "hello_llvm_std"
version = "0.1.0"

[toolchain]
linux = "llvm@20.1.7"
EOF

cat > src/main.cpp <<'EOF'
import std;

int main() {
    std::println("llvm std {}", 23);
    return 0;
}
EOF

"$MCPP" build --no-cache > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: llvm import-std build failed"
    exit 1
}

binary=$(find target -type f -path '*/bin/hello_llvm_std' | head -1)
[[ -n "$binary" && -x "$binary" ]] || {
    find target -maxdepth 5 -type f
    echo "FAIL: hello_llvm_std binary missing"
    exit 1
}

out=$("$binary")
[[ "$out" == "llvm std 23" ]] || {
    echo "FAIL: wrong runtime output: $out"
    exit 1
}

echo "OK"
