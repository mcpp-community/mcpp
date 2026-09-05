#!/usr/bin/env bash
# requires:
# (no capability: every assertion reads mcpp's own output about a manifest key.)
#
# 254_pack_library_platform_coverage.sh — `[package] platforms` is a support
# CLAIM, and `mcpp pack` is where it first becomes checkable against evidence:
# the legs in the package are the platforms it can actually serve.
#
# THE HARD PART IS NOT FINDING THE GAP, IT IS NOT SHOUTING ABOUT IT.
#
# Four comparisons exist and only two may be printed:
#
#   packed, not declared        always actionable — the manifest disclaims a
#                               platform the package demonstrably serves.
#   declared, not packed        actionable ONLY IF THIS HOST COULD HAVE BUILT IT.
#                               The normal release flow is one `mcpp pack` per
#                               platform in CI, so a Linux runner producing no
#                               macos leg is not an omission, it is every single
#                               run. A warning that fires on every run is one
#                               nobody reads, and then the real one is invisible
#                               too.
#
# So the silence is as much the feature as the warning, and both are asserted.
#
# THE PER-HOST PICKS BELOW MIRROR host_can_serve (registry.cppm:542-566).
# That is deliberate and it is a tripwire: if mcpp ever gains, say, a
# macOS-hosted Windows toolchain, this test starts failing — which is the
# correct outcome, because the table it encodes will have changed and the
# expectations here have to be re-derived rather than assumed.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p mathkit/src
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
export namespace mk { int answer(); }
EOF
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk { int answer() { return 42; } }
EOF

manifest() {   # $1 = the platforms array body, e.g. '"macos"'
    cat > "$TMP/mathkit/mcpp.toml" <<EOF
[package]
name      = "mathkit"
version   = "0.1.0"
platforms = [$1]
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
EOF
}

cd mathkit

# The host's own platform, taken from mcpp's answer rather than from `uname`:
# the build directory is named for the triple mcpp resolved, so this agrees with
# the code under test by construction. (Reading it from a regex over `--target`
# spellings is how an earlier test came to assert Linux expectations on macOS,
# whose triple has two segments and no env.)
manifest '"linux", "macos", "windows"'
rm -rf target
"$MCPP" pack mathkit > probe.log 2>&1 || { cat probe.log; echo "pack failed"; exit 1; }
host_triple="$(find target -mindepth 1 -maxdepth 1 -type d ! -name dist -exec basename {} \; | head -1)"
case "$host_triple" in
    *-linux-*|*-linux)   HOST_OS=linux   ;;
    *-macos*)            HOST_OS=macos   ;;
    *-windows-*)         HOST_OS=windows ;;
    *) echo "FAIL: could not read the host platform out of target/$host_triple"; exit 1 ;;
esac
echo "host platform: $HOST_OS (from target/$host_triple)"

# Per host: one platform this host CANNOT serve, and one it can but did not pack.
case "$HOST_OS" in
    linux)   UNSERVABLE=macos   SERVABLE_UNPACKED=windows ;;
    windows) UNSERVABLE=macos   SERVABLE_UNPACKED=linux   ;;
    # macOS serves exactly one target — "macOS has no Linux-targeting payload at
    # all", and PE needs a Windows or Linux host — so there is no
    # servable-but-unpacked platform to name here. Structural, not a gap.
    macos)   UNSERVABLE=windows SERVABLE_UNPACKED=        ;;
esac

# ── 1. packed, not declared ─────────────────────────────────────────────
manifest "\"$UNSERVABLE\""
rm -rf target
"$MCPP" pack mathkit > undeclared.log 2>&1 || { cat undeclared.log; echo "pack failed"; exit 1; }
grep -q "ships a $HOST_OS binary" undeclared.log || {
    cat undeclared.log
    echo "FAIL: the package ships a $HOST_OS artifact while [package] platforms lists"
    echo "      only $UNSERVABLE, and mcpp said nothing. The manifest disclaims a"
    echo "      platform the package serves, which is the claim consumers resolve against."
    exit 1; }

# ── 2. declared and unservable here: SILENCE ────────────────────────────
#
# The half that keeps the warning worth reading. Asserted on its own manifest so
# a stray match from case 1 cannot satisfy it.
manifest "\"$HOST_OS\", \"$UNSERVABLE\""
rm -rf target
"$MCPP" pack mathkit > quiet.log 2>&1 || { cat quiet.log; echo "pack failed"; exit 1; }
grep -q "claims $UNSERVABLE" quiet.log && {
    cat quiet.log
    echo "FAIL: mcpp asked for a $UNSERVABLE leg on a $HOST_OS host, which cannot"
    echo "      build one. That warning would fire on every release run of every"
    echo "      cross-platform package, and a warning that always fires hides the"
    echo "      one that matters."
    exit 1; }
grep -q "ships a $HOST_OS binary" quiet.log && {
    cat quiet.log
    echo "FAIL: $HOST_OS is declared AND packed, and mcpp still complained about it"
    exit 1; }

# ── 3. declared, servable here, not packed ──────────────────────────────
if [[ -n "$SERVABLE_UNPACKED" ]]; then
    manifest "\"$HOST_OS\", \"$SERVABLE_UNPACKED\""
    rm -rf target
    "$MCPP" pack mathkit > gap.log 2>&1 || { cat gap.log; echo "pack failed"; exit 1; }
    grep -q "claims $SERVABLE_UNPACKED" gap.log || {
        cat gap.log
        echo "FAIL: [package] platforms claims $SERVABLE_UNPACKED, this host can build"
        echo "      for it, no such leg was packed, and mcpp said nothing. Consumers"
        echo "      on $SERVABLE_UNPACKED resolve this package and find no artifact."
        exit 1; }
    echo "PASS: coverage gaps are reported, and unservable platforms are not"
else
    echo "NOTE: a $HOST_OS host serves exactly one target, so there is no"
    echo "      servable-but-unpacked platform to assert here. Case 3 did not run."
    echo "PASS: an undeclared platform is reported, an unservable one stays quiet"
fi
