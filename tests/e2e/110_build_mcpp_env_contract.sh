#!/usr/bin/env bash
# requires: gcc
# G3: build.mcpp environment contract — MCPP_TARGET/HOST/PROFILE/OUT_DIR/
# MANIFEST_DIR/FEATURES injected into the child; contract values are part of
# the cache key (a feature change re-runs the program; an identical build
# hits the cache).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new envbp > /dev/null
cd envbp

cat > build.mcpp <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
static const char* env_or(const char* n) { const char* v = std::getenv(n); return v ? v : "<unset>"; }
int main() {
    std::ofstream f("src/contract_gen.cpp");
    f << "extern \"C\" const char* bp_target()   { return \"" << env_or("MCPP_TARGET") << "\"; }\n";
    f << "extern \"C\" const char* bp_host()     { return \"" << env_or("MCPP_HOST") << "\"; }\n";
    f << "extern \"C\" const char* bp_profile()  { return \"" << env_or("MCPP_PROFILE") << "\"; }\n";
    f << "extern \"C\" const char* bp_features() { return \"" << env_or("MCPP_FEATURES") << "\"; }\n";
    f << "extern \"C\" int bp_has_extra() { return "
      << (std::getenv("MCPP_FEATURE_EXTRA") ? 1 : 0) << "; }\n";
    if (!f) return 1;
    // OUT_DIR must exist and be writable.
    const char* out = std::getenv("MCPP_OUT_DIR");
    if (!out) { std::fprintf(stderr, "MCPP_OUT_DIR unset\n"); return 1; }
    std::ofstream probe(std::string(out) + "/probe.txt");
    if (!probe) { std::fprintf(stderr, "MCPP_OUT_DIR not writable\n"); return 1; }
    std::printf("mcpp:generated=src/contract_gen.cpp\n");
    return 0;
}
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" const char* bp_target();
extern "C" const char* bp_host();
extern "C" const char* bp_profile();
extern "C" const char* bp_features();
extern "C" int bp_has_extra();
int main() {
    std::println("target={} host={} profile={} features=[{}] extra={}",
                 bp_target(), bp_host(), bp_profile(), bp_features(), bp_has_extra());
    return 0;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "envbp"
version = "0.1.0"

[features]
default = []
extra   = []
EOF

"$MCPP" build > build1.log 2>&1 || { cat build1.log; echo "build 1 failed"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
host_triple_re='[a-z0-9_]+-(linux|macos|windows)(-[a-z]+)?'
[[ "$out" =~ target=$host_triple_re\ host=$host_triple_re\ profile=dev\ features=\[\]\ extra=0 ]] || {
    echo "unexpected contract values: $out"; exit 1; }

# Identical build → no re-run (either the whole-build fast path short-circuits
# before build.mcpp, or the build.mcpp cache hits — both are fine; a "running"
# line is not).
"$MCPP" build > build2.log 2>&1
grep -q "build.mcpp running" build2.log && {
    cat build2.log; echo "identical build re-ran build.mcpp"; exit 1; } || true

# Feature change → contract hash changes → re-run, and the program observes
# the feature.
"$MCPP" build --features extra > build3.log 2>&1 || { cat build3.log; echo "build 3 failed"; exit 1; }
grep -q "build.mcpp running" build3.log || {
    cat build3.log; echo "feature change did not re-run build.mcpp"; exit 1; }
bin="$(ls -t $(find target -name 'envbp' -type f) | head -1)"
out="$("./$bin" | tail -1)"
[[ "$out" == *"features=[extra] extra=1"* ]] || {
    echo "feature not visible to build.mcpp: $out"; exit 1; }

# Profile change also re-runs.
"$MCPP" build --release > build4.log 2>&1 || { cat build4.log; echo "build 4 failed"; exit 1; }
grep -q "build.mcpp running" build4.log || {
    cat build4.log; echo "profile change did not re-run build.mcpp"; exit 1; }
bin="$(ls -t $(find target -name 'envbp' -type f) | head -1)"
out="$("./$bin" | tail -1)"
[[ "$out" == *"profile=release"* ]] || { echo "profile not visible: $out"; exit 1; }

echo "OK"
