#!/usr/bin/env bash
# requires: mingw-cross
#
# A cross build must not relink on every invocation.
#
# plan.cppm's target_output() used to spell the artifact suffix from
# mcpp::platform::exe_suffix — a HOST constant. Cross-compiling from Linux to
# a PE target, that yields `bin/foo` while mingw's GCC driver writes
# `bin/foo.exe`, so the file ninja was told to produce never appears. ninja
# finds the declared output missing on every run and reruns the link edge
# forever: incremental builds are effectively off for PE targets.
#
# The symptom is invisible to the other cross tests because they look for the
# REAL artifact (`find -name '*.exe'`), not for what ninja declared — both of
# their assertions hold while the inconsistency sits underneath them.
#
# See .agents/docs/2026-08-03-b3-target-aware-artifact-naming.md.
set -euo pipefail

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

mkdir -p src
cat > mcpp.toml <<'EOF'
[package]
name    = "relinkprobe"
version = "0.1.0"
EOF

cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF

TRIPLE=x86_64-windows-gnu

"$MCPP" build --target "$TRIPLE" > "$TMP/build1.log" 2>&1 || {
    echo "first cross build failed:"; cat "$TMP/build1.log"; exit 1; }

# The produced artifact — whatever it is actually called.
ART="$(find "target/$TRIPLE" -type f -path '*/bin/*' -name 'relinkprobe*' | head -1)"
[[ -n "$ART" ]] || { echo "no artifact produced under target/$TRIPLE"; exit 1; }

# ── The declared ninja output must be the file that actually gets written ────
# This is the root assertion. Positive grep on purpose: `! cmd | grep` is exempt
# from errexit and can never fail.
NINJA="$(find "target/$TRIPLE" -name build.ninja | head -1)"
[[ -n "$NINJA" ]] || { echo "no build.ninja found"; exit 1; }

DECLARED="$(grep -oE '^build [^:]*bin/relinkprobe[^ :]*' "$NINJA" | head -1 | sed 's/^build //' | tr -d ' ')"
[[ -n "$DECLARED" ]] || { echo "no link edge for relinkprobe in build.ninja"; exit 1; }

BUILDDIR="$(dirname "$NINJA")"
if [[ ! -f "$BUILDDIR/$DECLARED" ]]; then
    echo "FAIL: ninja declares output '$DECLARED' but that file does not exist"
    echo "      actually produced: $(basename "$ART")"
    echo "      => the link edge can never be satisfied, so it reruns every build"
    exit 1
fi

# ── And the observable consequence: a second build must not relink ──────────
T1="$(stat -c %Y "$ART" 2>/dev/null || stat -f %m "$ART")"
sleep 1   # ensure a relink would be visible at 1s mtime granularity
"$MCPP" build --target "$TRIPLE" > "$TMP/build2.log" 2>&1 || {
    echo "second cross build failed:"; cat "$TMP/build2.log"; exit 1; }
T2="$(stat -c %Y "$ART" 2>/dev/null || stat -f %m "$ART")"

if [[ "$T1" != "$T2" ]]; then
    echo "FAIL: artifact was relinked on an up-to-date rebuild ($T1 -> $T2)"
    echo "      ninja declared: $DECLARED"
    echo "      produced:       $(basename "$ART")"
    exit 1
fi

echo "OK: cross build declares the artifact it actually produces, and does not relink"
