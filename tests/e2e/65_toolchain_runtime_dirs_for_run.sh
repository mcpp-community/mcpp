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

source "$(dirname "$0")/_llvm_env.sh"

if [[ ! -x "$LLVM_ROOT/bin/clang++" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION} is not installed"
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
name    = "toolchain_runtime_env"
version = "0.1.0"

[toolchain]
linux = "llvm@${LLVM_VERSION}"

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
    if (path.find("@LLVM_LIB_SUBSTR@") == std::string::npos) return 11;
    if (path.find("xim-x-glibc/2.39/lib64") == std::string::npos) return 12;
    return 0;
}
EOF
# Inject the resolved LLVM version into the quoted heredoc via a placeholder.
sed -i "s|@LLVM_LIB_SUBSTR@|xim-x-llvm/${LLVM_VERSION}/lib|" src/main.cpp

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
