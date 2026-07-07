#!/usr/bin/env bash
# requires: import-std-libcxx
# 41_llvm_std_compat.sh — build a project that uses import std.compat with Clang.
set -e

OS="$(uname -s)"
# libc++ std.compat.cppm is only available on Linux/macOS — on Windows there
# is no libc++ module distribution. Exit gracefully; the import-std-libcxx
# capability check in run_all.sh already gates this, but guard here too.
if [[ "$OS" == MINGW* || "$OS" == MSYS* || "$OS" == CYGWIN* ]]; then
    echo "SKIP: libc++ std.compat.cppm not available on Windows"
    exit 0
fi

source "$(dirname "$0")/_llvm_env.sh"

if [[ ! -x "$LLVM_ROOT/bin/clang++" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION} is not installed"
    exit 0
fi
if [[ ! -f "$LLVM_ROOT/share/libc++/v1/std.compat.cppm" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION} has no libc++ std.compat.cppm"
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
name    = "compat_test"
version = "0.1.0"
[toolchain]
linux = "llvm@${LLVM_VERSION}"
EOF

cat > src/main.cpp <<'EOF'
import std.compat;

int main() {
    // std.compat provides C stdlib functions like printf
    printf("compat %d\n", 42);
    return 0;
}
EOF

"$MCPP" build --no-cache > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: std.compat build failed"
    exit 1
}

binary=$(find target -type f -path '*/bin/compat_test' | head -1)
[[ -n "$binary" && -x "$binary" ]] || {
    echo "FAIL: compat_test binary missing"
    exit 1
}

out=$("$binary")
[[ "$out" == "compat 42" ]] || {
    echo "FAIL: wrong output: $out"
    exit 1
}

echo "OK"
