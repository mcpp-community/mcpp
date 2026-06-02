#!/usr/bin/env bash
# requires: llvm linux
# dlopen() providers such as GLX drivers do not use the main executable's
# RUNPATH for their own DT_NEEDED closure. mcpp run must therefore expose the
# toolchain runtime directories in LD_LIBRARY_PATH as well.
set -e

OS="$(uname -s)"
if [[ "$OS" != "Linux" ]]; then
    echo "SKIP: LD_LIBRARY_PATH runtime-dir check is Linux-specific"
    exit 0
fi

LLVM_ROOT="${HOME}/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7"
if [[ ! -x "$LLVM_ROOT/bin/clang++" ]]; then
    echo "SKIP: xlings llvm@20.1.7 is not installed"
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
name    = "toolchain_runtime_env"
version = "0.1.0"

[toolchain]
linux = "llvm@20.1.7"

[targets.toolchain_runtime_env]
kind = "bin"
main = "src/main.cpp"
EOF

cat > src/main.cpp <<'EOF'
#include <cstdlib>
#include <string>

int main() {
    const char* value = std::getenv("LD_LIBRARY_PATH");
    if (value == nullptr) return 10;

    std::string path(value);
    if (path.find("xim-x-llvm/20.1.7/lib") == std::string::npos) return 11;
    if (path.find("xim-x-glibc/2.39/lib64") == std::string::npos) return 12;
    return 0;
}
EOF

"$MCPP" build > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: build failed"
    exit 1
}

"$MCPP" run > "$TMP/run.log" 2>&1 || {
    cat "$TMP/run.log"
    echo "FAIL: mcpp run did not expose toolchain runtime dirs"
    exit 1
}

echo "OK"
