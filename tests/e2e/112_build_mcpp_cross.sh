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

# ── host≠target for `import std;` in build.mcpp ────────────────────────────
# The std module staged for a build.mcpp must be the HOST one. Feeding it the
# target's would produce a helper that cannot execute here, and the failure is
# silent until exec time — the same class of mistake the mingw-cross work had
# to fix in four separate places. A cross build is the only configuration
# where host and target BMIs differ, so this is the one place it can be
# caught.
cat > build.mcpp <<'EOF'
import std;
int main() {
    std::ofstream f("src/cross_gen.cpp");
    f << "extern \"C\" const char* bp_target() { return \""
      << (std::getenv("MCPP_TARGET") ? std::getenv("MCPP_TARGET") : "<unset>")
      << "\"; }\n";
    if (!f) return 1;
    std::println("mcpp:generated=src/cross_gen.cpp");
    return 0;
}
EOF

rm -f src/cross_gen.cpp
"$MCPP" build --target x86_64-windows-gnu > build-std.log 2>&1 || {
    cat build-std.log; echo "cross build with import std in build.mcpp failed"; exit 1; }
# The helper actually RAN on the host — proven by the file it was asked to
# write, not by the compiler's exit code.
[[ -f src/cross_gen.cpp ]] || {
    cat build-std.log; echo "import-std build.mcpp did not run on the host"; exit 1; }
grep -q 'x86_64-windows-gnu' src/cross_gen.cpp || {
    cat src/cross_gen.cpp; echo "MCPP_TARGET wrong under import std"; exit 1; }

echo "OK"
