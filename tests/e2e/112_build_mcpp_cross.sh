#!/usr/bin/env bash
# requires: gcc mingw-cross
# G3 (cross): build.mcpp under `--target x86_64-windows-gnu` is no longer
# skipped — it compiles AND runs on the host (host-resolved toolchain) and
# sees MCPP_TARGET = the cross triple while MCPP_HOST stays the host.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new crossbp > /dev/null
cd crossbp

cat > build.mcpp <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <fstream>
static const char* env_or(const char* n) { const char* v = std::getenv(n); return v ? v : "<unset>"; }
int main() {
    std::ofstream f("src/cross_gen.cpp");
    f << "extern \"C\" const char* bp_target() { return \"" << env_or("MCPP_TARGET") << "\"; }\n";
    if (!f) return 1;
    std::printf("mcpp:generated=src/cross_gen.cpp\n");
    return 0;
}
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" const char* bp_target();
int main() {
    std::println("bp target = {}", bp_target());
    return 0;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "crossbp"
version = "0.1.0"
EOF

"$MCPP" build --target x86_64-windows-gnu > build.log 2>&1 || {
    cat build.log; echo "cross build failed"; exit 1; }
# Not skipped anymore.
grep -qi "skipped under a cross" build.log && {
    echo "build.mcpp still skipped under cross"; exit 1; } || true
# The generated source baked in the CROSS triple (contract value), and the
# produced artifact is a PE binary.
grep -q 'x86_64-windows-gnu' src/cross_gen.cpp || {
    cat src/cross_gen.cpp; echo "MCPP_TARGET was not the cross triple"; exit 1; }
exe="$(find target -name 'crossbp.exe' | head -1)"
[[ -n "$exe" ]] || { echo "no PE artifact produced"; exit 1; }

# Wine run (when available) proves the full loop.
if command -v wine &>/dev/null; then
    out="$(wine "$exe" 2>/dev/null | tr -d '\r' | tail -1)"
    [[ "$out" == "bp target = x86_64-windows-gnu" ]] || {
        echo "unexpected wine output: $out"; exit 1; }
fi

echo "OK"
