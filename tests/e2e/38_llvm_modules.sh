#!/usr/bin/env bash
# 38_llvm_modules.sh — multi-module project with LLVM/Clang.
#
# Tests: module interface (.cppm) with `export module`, cross-module import,
# dyndep pipeline, BMI path parameterization (pcm.cache/*.pcm), and
# -fmodule-output / -fprebuilt-module-path flags.
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
name    = "llvm_modules"
version = "0.1.0"

[toolchain]
linux = "llvm@20.1.7"
EOF

# Module interface unit: exports greet()
cat > src/greet.cppm <<'EOF'
export module llvm_modules.greet;

import std;

export std::string greet(std::string_view name) {
    return std::format("Hello, {}!", name);
}
EOF

# Main imports the module
cat > src/main.cpp <<'EOF'
import std;
import llvm_modules.greet;

int main() {
    std::println("{}", greet("clang"));
    return 0;
}
EOF

"$MCPP" build --no-cache > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: llvm multi-module build failed"
    exit 1
}

binary=$(find target -type f -path '*/bin/llvm_modules' | head -1)
[[ -n "$binary" && -x "$binary" ]] || {
    find target -maxdepth 5 -type f
    echo "FAIL: llvm_modules binary missing"
    exit 1
}

out=$("$binary")
[[ "$out" == "Hello, clang!" ]] || {
    echo "FAIL: wrong runtime output: $out"
    exit 1
}

# Verify BMI went to pcm.cache/, not gcm.cache/
pcm_dir=$(find target -type d -name 'pcm.cache' | head -1)
[[ -n "$pcm_dir" ]] || {
    echo "FAIL: pcm.cache/ directory not found (Clang should use pcm.cache)"
    exit 1
}
gcm_dir=$(find target -type d -name 'gcm.cache' | head -1)
[[ -z "$gcm_dir" ]] || {
    echo "FAIL: gcm.cache/ directory found (Clang should NOT use gcm.cache)"
    exit 1
}

echo "OK"
