#!/usr/bin/env bash
# requires: gcc
# #253: [features].<name>.flags — per-feature per-glob compile flags.
# Quadrants: feature off/on × build/test (0.0.94 dual-path invariant), plus:
#   - feature OFF: the rule does not exist → NO dead-glob warning (the issue's
#     opencv mlas noise), and the define is absent;
#   - feature ON: the define reaches EXACTLY the glob-matched TU (not the
#     package's other TUs — contrast feature `defines`, which are package-wide);
#   - feature ON with a dead glob: warning fires, naming the owning feature;
#   - flags do NOT propagate to a consumer (contrast feature `defines`, which
#     interface-propagate) — asserted in the dep half below.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new featflags > /dev/null
cd featflags

mkdir -p src/simd
cat > src/simd/kernel.cpp <<'EOF'
#ifndef KERNEL_TAG
#error "feature flags did not reach the glob-matched TU"
#endif
extern "C" int kernel_tag() { return KERNEL_TAG; }
EOF

cat > src/plain.cpp <<'EOF'
#ifdef KERNEL_TAG
#error "feature flags leaked outside their glob (must be per-TU, not package-wide)"
#endif
extern "C" int plain_ok() { return 1; }
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" int plain_ok();
#ifdef MCPP_FEATURE_SIMD
extern "C" int kernel_tag();
#endif
int main() {
#ifdef MCPP_FEATURE_SIMD
    std::println("tag = {}", kernel_tag() + plain_ok());
#else
    std::println("tag = none");
#endif
    return 0;
}
EOF

# Base sources deliberately do NOT cover src/simd/** (the opencv shape: a
# feature's sources live only under the feature) — so the gated TU is absent
# in every feature-off quadrant, including `mcpp test`, whose drop-skip only
# applies to gated globs that base sources also cover.
cat > mcpp.toml <<'EOF'
[package]
name    = "featflags"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[features]
default = []
simd    = { sources = ["src/simd/**"], flags = [
              { glob = "src/simd/**", defines = ["KERNEL_TAG=6"] } ] }
dead    = { flags = [ { glob = "src/never/**", defines = ["NOPE"] } ] }
EOF

# Quadrant 1: feature OFF, build — rule doesn't exist, so no dead-glob warning.
"$MCPP" build > build_off.log 2>&1 || { cat build_off.log; echo "build (off) failed"; exit 1; }
grep -q "matched no source file" build_off.log && {
    cat build_off.log; echo "feature-off build must not warn about feature flag globs"; exit 1; } || true
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "tag = none" ]] || { echo "unexpected (off): $out"; exit 1; }

# Quadrant 2: feature ON, build — define lands on the matched TU only.
"$MCPP" build --features simd > build_on.log 2>&1 || { cat build_on.log; echo "build (on) failed"; exit 1; }
bin="$(ls -t $(find target -name 'featflags' -type f) | head -1)"
[[ -n "$bin" ]] || { echo "binary not found"; exit 1; }
out="$("./$bin" | tail -1)"
[[ "$out" == "tag = 7" ]] || { echo "unexpected (on): $out (want 6+1)"; exit 1; }

# Active feature with a glob that matches nothing → warning naming the feature.
"$MCPP" build --features simd,dead > build_dead.log 2>&1 || {
    cat build_dead.log; echo "build (dead) failed"; exit 1; }
grep -q "features.dead.flags glob 'src/never/\*\*' matched no source file" build_dead.log || {
    cat build_dead.log; echo "missing feature-scoped dead-glob warning"; exit 1; }

# Quadrants 3+4: test path — flags behavior must match build (dual-path).
mkdir -p tests
cat > tests/test_featflags.cpp <<'EOF'
#ifdef MCPP_FEATURE_SIMD
extern "C" int kernel_tag();
int main() { return kernel_tag() == 6 ? 0 : 1; }
#else
int main() { return 0; }
#endif
EOF
"$MCPP" test > test_off.log 2>&1 || { cat test_off.log; echo "mcpp test (off) failed"; exit 1; }
grep -q "matched no source file" test_off.log && {
    cat test_off.log; echo "feature-off test must not warn about feature flag globs"; exit 1; } || true
"$MCPP" test --features simd > test_on.log 2>&1 || { cat test_on.log; echo "mcpp test (on) failed"; exit 1; }

# --- Dep half: feature flags are PRIVATE to the owning package. ---
# The dep's feature carries both an interface define (propagates) and a
# per-glob flag define (must NOT propagate to the consumer's TUs).
cd "$TMP"
mkdir -p widget/src app/src
cat > widget/mcpp.toml <<'EOF'
[package]
name    = "widget"
version = "0.1.0"

[features]
default = []
turbo   = { defines = ["WIDGET_IFACE=1"], flags = [
              { glob = "src/**", defines = ["WIDGET_PRIVATE=1"] } ] }

[targets.widget]
kind = "lib"
EOF
cat > widget/src/widget.cppm <<'EOF'
export module widget;
#ifndef WIDGET_PRIVATE
#error "feature flags did not reach the dep's own matched TU"
#endif
export int widget_anchor() { return WIDGET_PRIVATE; }
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
widget = { path = "../widget", features = ["turbo"] }
EOF
cat > app/src/main.cpp <<'EOF'
import widget;
#ifndef WIDGET_IFACE
#error "feature defines must interface-propagate to the consumer"
#endif
#ifdef WIDGET_PRIVATE
#error "feature flags must NOT propagate to the consumer"
#endif
int main() { return widget_anchor() == 1 ? 0 : 1; }
EOF
cd app
"$MCPP" build > build_dep.log 2>&1 || { cat build_dep.log; echo "dep build failed"; exit 1; }
"$MCPP" run > /dev/null 2>&1 || { echo "dep run failed"; exit 1; }

echo "OK"
