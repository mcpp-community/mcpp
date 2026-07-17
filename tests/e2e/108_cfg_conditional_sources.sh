#!/usr/bin/env bash
# requires: gcc
# G1b: [target.'cfg(...)'.build] sources — target-conditional source globs,
# evaluated against the RESOLVED target. Host-aware: asserts the host-arch
# gated file is compiled and the other-arch file is not.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    x86_64)  THIS=x86_64;  OTHER=aarch64 ;;
    aarch64|arm64) THIS=aarch64; OTHER=x86_64 ;;
    *) echo "SKIP: unmapped arch $HOST_ARCH"; exit 0 ;;
esac

cd "$TMP"
"$MCPP" new condsrc > /dev/null
cd condsrc

mkdir -p arch
cat > arch/this.cpp <<'EOF'
extern "C" int arch_answer() { return 7; }
EOF
cat > arch/other.cpp <<'EOF'
extern "C" int arch_answer() { return 8; }   // would double-define if both compiled
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" int arch_answer();
int main() {
    std::println("arch = {}", arch_answer());
    return 0;
}
EOF

cat > mcpp.toml <<EOF
[package]
name    = "condsrc"
version = "0.1.0"

[target.'cfg(arch = "$THIS")'.build]
sources = ["arch/this.cpp"]

[target.'cfg(arch = "$OTHER")'.build]
sources = ["arch/other.cpp"]
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "arch = 7" ]] || { echo "unexpected output: $out"; exit 1; }
# The other-arch file must not have been compiled.
find target -name 'other.o' | grep -q . && { echo "other-arch source compiled"; exit 1; } || true

echo "OK"
