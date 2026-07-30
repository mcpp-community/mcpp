#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 173_build_profile_isolation.sh — the build profile must be an invalidation
# axis, and the fast path must not hand back another profile's artifacts.
#
# `[profile.<name>]` lands its knobs in buildConfig.optLevel/debug/lto/strip and
# flags.cppm turns them into -O<n>/-g/-flto, but the fingerprint serialized only
# cflags/cxxflags/ldflags. So `--dev`, `--release` and `--profile dist` produced
# ONE fingerprint, hence one target/<triple>/<fp>/ directory and one global cache
# entry — a release build could be served -O0 -g dependency objects.
#
# The second half was worse and user-visible without any cache involved:
# `.build_cache` keyed its fast-path entries by target triple alone, and the fast
# path only refuses to run when an EXPLICIT --profile/--dev/--release is passed.
# A bare `mcpp build` after `mcpp build --release` therefore took the fast path
# against the release build.ninja and reported success in 0.00s, leaving -O2
# artifacts where -O0 -g was asked for.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

cd "$TMP"
mkdir -p app/src
cd app
cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::printf("hi\n"); return 0; }
EOF
cat > mcpp.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"
standard = "c++23"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

# The -O level recorded in a build dir's build.ninja. Reading the generated graph
# rather than a status line: a status line is what lied here before.
opt_of() { grep -oE '\-O[0-9s]' "$1/build.ninja" | sort -u | tr -d '\n'; }
dir_for_opt() {
    local want="$1"
    for d in target/*/*/; do
        [[ -f "$d/build.ninja" ]] || continue
        [[ "$(opt_of "$d")" == "$want" ]] && { printf '%s' "$d"; return 0; }
    done
    return 1
}

# ── each profile gets its own build dir ─────────────────────────────────────
"$MCPP" build --dev     > dev.log     2>&1 || { cat dev.log; exit 1; }
"$MCPP" build --release > release.log 2>&1 || { cat release.log; exit 1; }
"$MCPP" build --profile dist > dist.log 2>&1 || { cat dist.log; exit 1; }

count=$(find target -name build.ninja | wc -l)
[[ "$count" -eq 3 ]] || {
    echo "FAIL: expected one build dir per profile, found $count"
    for d in target/*/*/; do echo "  $d -> $(opt_of "$d")"; done
    exit 1
}
dir_for_opt "-O0" >/dev/null || { echo "FAIL: no -O0 (dev) build dir"; exit 1; }
dir_for_opt "-O2" >/dev/null || { echo "FAIL: no -O2 (release) build dir"; exit 1; }
dir_for_opt "-O3" >/dev/null || { echo "FAIL: no -O3 (dist) build dir"; exit 1; }

# The announced profile must be the one that was built.
grep -q 'Finished dev' dev.log         || { cat dev.log; echo "FAIL: dev not announced"; exit 1; }
grep -q 'Finished release' release.log || { cat release.log; echo "FAIL: release not announced"; exit 1; }
grep -q 'Finished dist' dist.log       || { cat dist.log; echo "FAIL: dist not announced"; exit 1; }
# ...and the descriptor must not contradict the flags. "release [optimized]" was
# hardcoded, so a --dev build used to announce itself as an optimized release.
grep -q 'Finished dev \[unoptimized' dev.log || {
    cat dev.log
    echo "FAIL: dev build described as optimized"
    exit 1
}

# ── the regression gate: bare build after --release must be dev ─────────────
DEVDIR="$(dir_for_opt "-O0")"
rm -rf target
"$MCPP" build --release > r2.log 2>&1 || { cat r2.log; exit 1; }
"$MCPP" build           > bare.log 2>&1 || { cat bare.log; exit 1; }

grep -q 'Finished dev' bare.log || {
    echo "FAIL: bare build after --release did not resolve to the dev profile"
    cat bare.log
    exit 1
}
DEVDIR="$(dir_for_opt "-O0")" || {
    echo "FAIL: bare build produced no -O0 build dir (fast path served release)"
    for d in target/*/*/; do echo "  $d -> $(opt_of "$d")"; done
    cat bare.log
    exit 1
}
# The dev objects must actually exist and carry debug info: an existing build dir
# with no objects in it would satisfy a path check but not a build.
[[ -f "$DEVDIR/obj/main.o" ]] || {
    echo "FAIL: dev build dir has no object"
    find "$DEVDIR" -type f | head
    exit 1
}
if command -v readelf > /dev/null 2>&1; then
    readelf --debug-dump=info "$DEVDIR/obj/main.o" 2>/dev/null \
        | grep -q 'DW_AT_producer' || {
        echo "FAIL: dev object has no debug info (it is a release object)"
        exit 1
    }
fi

# ── switching back is incremental, not a full rebuild ──────────────────────
# Both entries coexist in .build_cache now; keying on the triple alone made each
# profile evict the other, so a dev/release rotation could never be incremental.
"$MCPP" build --release > r3.log 2>&1 || { cat r3.log; exit 1; }
"$MCPP" build           > bare2.log 2>&1 || { cat bare2.log; exit 1; }
if grep -qE 'src/main\.cpp' bare2.log; then
    echo "FAIL: returning to the dev profile recompiled from scratch"
    cat bare2.log
    exit 1
fi
grep -q 'Finished dev' bare2.log || { cat bare2.log; echo "FAIL: wrong profile"; exit 1; }

echo "OK"
