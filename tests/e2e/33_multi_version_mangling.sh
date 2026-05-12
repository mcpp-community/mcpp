#!/usr/bin/env bash
# 33_multi_version_mangling.sh — Level 1 of dep resolution: when two
# transitive consumers want incompatible (non-overlapping) versions of
# the same package, the secondary copy is rewritten to use a mangled
# module name so both BMIs coexist in the build graph.
#
# Setup: libA pinned to cmdline 0.0.1, libB pinned to cmdline 0.0.2.
# Both live as path-deps of `app`. The SemVer merger has nothing to
# combine (=0.0.1 ⨯ =0.0.2 has no satisfying version), so the resolver
# stages cmdline 0.0.2 + libB under `target/.mangled/` with rewritten
# `module/import` declarations and the build proceeds.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

# Index + xpkgs need to be visible since we exercise version-source deps.
mkdir -p "$MCPP_HOME/registry/data"
# Link pre-cached index data (may be under old "mcpp-index" or new "mcpplibs" name)
for idx_name in mcpplibs mcpp-index; do
    if [[ -d "$HOME/.mcpp/registry/data/$idx_name" ]]; then
        ln -sf "$HOME/.mcpp/registry/data/$idx_name" \
            "$MCPP_HOME/registry/data/$idx_name"
    fi
done
if [[ -d "$HOME/.mcpp/registry/data/xpkgs" ]]; then
    [[ -e "$MCPP_HOME/registry/data/xpkgs" ]] \
        || ln -sf "$HOME/.mcpp/registry/data/xpkgs" \
            "$MCPP_HOME/registry/data/xpkgs"
fi

# ── libA: pinned to cmdline 0.0.1 ──────────────────────────────────────
mkdir -p "$TMP/libA" && cd "$TMP/libA"
"$MCPP" new libA > /dev/null
cd libA
rm -f src/main.cpp
cat > src/libA.cppm <<'EOF'
export module libA;
import mcpplibs.cmdline;   // pulls cmdline 0.0.1 (primary)
import std;
export int libA_v() { return 1; }
EOF
cat > mcpp.toml <<'EOF'
[package]
name    = "libA"
version = "0.1.0"
[targets.libA]
kind = "lib"

[dependencies.mcpplibs]
cmdline = "=0.0.1"
EOF

# ── libB: pinned to cmdline 0.0.2 (incompatible with libA) ─────────────
mkdir -p "$TMP/libB" && cd "$TMP/libB"
"$MCPP" new libB > /dev/null
cd libB
rm -f src/main.cpp
cat > src/libB.cppm <<'EOF'
export module libB;
import mcpplibs.cmdline;   // resolver rewrites this to the mangled secondary
import std;
export int libB_v() { return 2; }
EOF
cat > mcpp.toml <<'EOF'
[package]
name    = "libB"
version = "0.1.0"
[targets.libB]
kind = "lib"

[dependencies.mcpplibs]
cmdline = "=0.0.2"
EOF

# ── app: pulls both libs ───────────────────────────────────────────────
mkdir -p "$TMP/app" && cd "$TMP/app"
"$MCPP" new app > /dev/null
cd app
cat > src/main.cpp <<'EOF'
import std;
import libA;
import libB;
int main() {
    std::println("a={} b={}", libA_v(), libB_v());
    return libA_v() + libB_v() == 3 ? 0 : 1;
}
EOF
cat > mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies]
libA = { path = "$TMP/libA/libA" }
libB = { path = "$TMP/libB/libB" }
EOF

"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "mangling build failed"; exit 1; }

# Sanity: the resolver should announce the mangling step. Either
# `Mangled` (our trace) or the mangled module name landing in the
# ninja file is fine.
grep -q 'Mangled.*cmdline' build.log || {
    cat build.log
    echo "expected 'Mangled' trace in build log"; exit 1; }

# Look for the mangled BMI on disk (proof both versions actually built).
find target -name 'mcpplibs.cmdline__v0_0_2__mcpp.gcm' | grep -q . || {
    echo "mangled BMI mcpplibs.cmdline__v0_0_2__mcpp.gcm not found"
    find target -name '*.gcm' | head -10
    exit 1; }
find target -name 'mcpplibs.cmdline.gcm' | grep -q . || {
    echo "primary BMI mcpplibs.cmdline.gcm not found"
    find target -name '*.gcm' | head -10
    exit 1; }

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "a=1 b=2" ]] || { echo "unexpected output: $out"; exit 1; }

echo "OK"
