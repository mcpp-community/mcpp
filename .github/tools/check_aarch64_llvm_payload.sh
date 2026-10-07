#!/usr/bin/env bash
# Positive admission for the native Linux ARM64 LLVM payload.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(uname -m)" == aarch64 ]] || {
    echo 'FAIL: this gate requires a native Linux aarch64 host'; exit 1;
}
MCPP="${MCPP:-mcpp}"
"$MCPP" toolchain install llvm 23.1.3
MCPP_E2E_LLVM_VERSION=23.1.3 source tests/e2e/_toolchain_env.sh
[[ -x "$LLVM_ROOT/bin/clang++" ]] || { echo 'FAIL: LLVM frontend missing'; exit 1; }
file -L "$LLVM_ROOT/bin/clang++" | grep -q 'ARM aarch64' || {
    echo 'FAIL: LLVM frontend is not native ARM64'; exit 1;
}
"$LLVM_ROOT/bin/clang++" --version
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export MCPP_HOME="$work/mcpp-home"
"$MCPP" self config --mirror "${MCPP_E2E_MIRROR:-GLOBAL}"
"$MCPP" self env --format json | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["data"]["defaultToolchain"] == "llvm@23.1.3", d'
"$MCPP" new "$work/native-probe"
cd "$work/native-probe"
"$MCPP" build
"$MCPP" run
binary="$(find target/aarch64-linux-gnu -type f -path '*/bin/native-probe' | head -1)"
[[ -n "$binary" ]] || { echo 'FAIL: default did not produce a native GNU artifact'; exit 1; }
readelf -l "$binary" | grep -q 'ld-linux-aarch64.so.1' || {
    echo 'FAIL: native default is not glibc-linked'; exit 1;
}
"$MCPP" build --target aarch64-linux-musl --toolchain gcc@16.1.0-musl
"$MCPP" run --target aarch64-linux-musl --toolchain gcc@16.1.0-musl
printf '%s\n' 'PASS: native ARM64 LLVM installs, builds and runs'
