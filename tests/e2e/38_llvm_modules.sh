#!/usr/bin/env bash
# requires: import-std-libcxx
# 38_llvm_modules.sh — multi-module project with LLVM/Clang.
#
# Tests: module interface (.cppm) with `export module`, cross-module import,
# dyndep pipeline, BMI path parameterization (pcm.cache/*.pcm), and
# -fmodule-output / -fprebuilt-module-path flags.
set -e

OS="$(uname -s)"
# libc++ std.cppm is only available on Linux/macOS — on Windows there is no
# libc++ module distribution. Exit gracefully; the import-std-libcxx capability
# check in run_all.sh already gates this, but guard here too for direct runs.
if [[ "$OS" == MINGW* || "$OS" == MSYS* || "$OS" == CYGWIN* ]]; then
    echo "SKIP: libc++ std.cppm not available on Windows"
    exit 0
fi

source "$(dirname "$0")/_llvm_env.sh"

if [[ ! -x "$LLVM_ROOT/bin/clang++" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION} is not installed"
    exit 0
fi
if [[ ! -f "$LLVM_ROOT/share/libc++/v1/std.cppm" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION} has no libc++ std.cppm"
    exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<EOF
[package]
name    = "llvm_modules"
version = "0.1.0"

[toolchain]
linux = "llvm@${LLVM_VERSION}"
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
