#!/usr/bin/env bash
# requires:
# 36_llvm_toolchain.sh — build a non-module C/C++ package with xlings LLVM.
set -e

source "$(dirname "$0")/_llvm_env.sh"

OS="$(uname -s)"
# On Windows the clang++ binary has a .exe suffix
if [[ "$OS" == MINGW* || "$OS" == MSYS* || "$OS" == CYGWIN* ]]; then
    CLANGPP_BIN="$LLVM_ROOT/bin/clang++.exe"
else
    CLANGPP_BIN="$LLVM_ROOT/bin/clang++"
fi
if [[ ! -x "$CLANGPP_BIN" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION} is not installed"
    exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

if [[ "$OS" == MINGW* || "$OS" == MSYS* || "$OS" == CYGWIN* ]]; then
    TC_KEY="windows"
else
    TC_KEY="linux"
fi

cat > mcpp.toml <<EOF
[package]
name    = "hello_llvm"
version = "0.1.0"

[language]
import_std = false

[toolchain]
${TC_KEY} = "llvm@${LLVM_VERSION}"
EOF

cat > src/main.cpp <<'EOF'
#include <iostream>

extern "C" int answer(void);

int main() {
    std::cout << "llvm " << answer() << "\n";
    return 0;
}
EOF

cat > src/answer.c <<'EOF'
int answer(void) {
    return 42;
}
EOF

"$MCPP" toolchain list > "$TMP/list.log" 2>&1
grep -q "llvm ${LLVM_VERSION}" "$TMP/list.log" || {
    cat "$TMP/list.log"
    echo "FAIL: toolchain list did not show installed llvm ${LLVM_VERSION}"
    exit 1
}

"$MCPP" build --no-cache > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: llvm build failed"
    exit 1
}
grep -q 'Finished' "$TMP/build.log" || {
    cat "$TMP/build.log"
    echo "FAIL: build did not finish"
    exit 1
}

if [[ "$OS" == MINGW* || "$OS" == MSYS* || "$OS" == CYGWIN* ]]; then
    binary=$(find target -type f -path '*/bin/hello_llvm.exe' | head -1)
else
    binary=$(find target -type f -path '*/bin/hello_llvm' | head -1)
fi
[[ -n "$binary" && -x "$binary" ]] || {
    find target -maxdepth 5 -type f
    echo "FAIL: hello_llvm binary missing"
    exit 1
}

out=$("$binary")
[[ "$out" == "llvm 42" ]] || {
    echo "FAIL: wrong runtime output: $out"
    exit 1
}

echo "OK"
