#!/usr/bin/env bash
# requires: msvc mingw
# 256_pack_library_fat_windows.sh — a fat package on Windows: an MSVC leg and a
# MinGW leg in ONE package, each selected by its own predicate.
#
# WHY THIS IS THE SHARPEST VERSION OF THE TEST. 245 packs `linux-gnu` +
# `linux-musl`, and both legs are called `libmathkit.a`. Here they are not:
#
#   x86_64-windows-msvc   mathkit.lib      (lib.exe, no `lib` prefix)
#   x86_64-windows-gnu    libmathkit.a     (ar, GNU naming)
#
# Two different filenames for the same target in one package is the plainest
# possible evidence that `lib/` has to be keyed by TRIPLE rather than by
# platform — a layout keyed by "windows" could hold only one of these, and the
# one it held would link for exactly half of its consumers.
#
# WHY IT DID NOT EXIST BEFORE, AND WHY THAT WAS THE WRONG CALL. This
# combination was reported as "not possible on Windows". It is: host_can_serve
# (registry.cppm:542-566) grants a Windows host `*-windows-msvc`,
# `*-windows-gnu` AND host-arch `*-linux-musl` — three targets, so a fat package
# needs no cross-compilation trickery at all. What was actually missing was the
# MinGW payload in the Windows e2e job. macOS is the genuinely impossible case:
# it serves exactly one target ("macOS has no Linux-targeting payload at all"),
# so there is no second leg to pack there under any circumstances.
set -e
source "$(dirname "$0")/_host_path.sh"

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
cat > mathkit/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
EOF

# The host arch, from mcpp's own answer rather than assumed: an aarch64 Windows
# runner would make every hardcoded `x86_64-` here a silent skip of the real work.
cd mathkit
"$MCPP" build > probe.log 2>&1 || { cat probe.log; echo "probe build failed"; exit 1; }
host_triple="$(find target -mindepth 1 -maxdepth 1 -type d ! -name dist -exec basename {} \; | head -1)"
ARCH="${host_triple%%-*}"
[[ -n "$ARCH" && "$host_triple" == *windows* ]] || {
    echo "FAIL: expected a windows host triple, read '$host_triple'"; exit 1; }
rm -rf target

MSVC_LEG="$ARCH-windows-msvc"
MINGW_LEG="$ARCH-windows-gnu"

"$MCPP" pack mathkit --target "$MSVC_LEG" --target "$MINGW_LEG" > pack.log 2>&1 \
    || { cat pack.log; echo "fat pack failed"; exit 1; }

pkg="$TMP/mathkit/target/dist/mathkit-0.1.0"
PKG_HOST="$(host_path "$pkg")"

# ── the two legs, under their two different names ───────────────────────
[[ -f "$pkg/lib/$MSVC_LEG/mathkit.lib" ]] || {
    find "$pkg" -type f
    echo "FAIL: the MSVC leg is not lib/$MSVC_LEG/mathkit.lib"
    exit 1; }
[[ -f "$pkg/lib/$MINGW_LEG/libmathkit.a" ]] || {
    find "$pkg" -type f
    echo "FAIL: the MinGW leg is not lib/$MINGW_LEG/libmathkit.a"
    exit 1; }
# Stated as an inequality too, because "both files exist" would still pass if the
# packer had put the same artifact in both directories.
[[ "$(basename "$pkg/lib/$MSVC_LEG/mathkit.lib")" \
   != "$(basename "$pkg/lib/$MINGW_LEG/libmathkit.a")" ]] || {
    echo "FAIL: the two legs ended up with the same filename"; exit 1; }

# Selected by predicate, never by a bare triple (inert on a native build — the
# defect 245's header describes).
grep -q "target\.'cfg(" "$pkg/mcpp.toml" || {
    cat "$pkg/mcpp.toml"; echo "FAIL: legs are not selected by cfg() predicates"; exit 1; }
grep -qE "^\[target\.'$ARCH-" "$pkg/mcpp.toml" && {
    cat "$pkg/mcpp.toml"
    echo "FAIL: a leg is selected by a BARE TRIPLE, which is inert on a native build"
    exit 1; }
# An env axis must appear: `os = "windows"` alone cannot separate these two legs,
# and a predicate that cannot separate them would hand MSVC consumers the MinGW
# archive. Matched with optional spaces — the emitted spelling is `env = "msvc"`,
# and the first version of this grep looked for `env=`, which is nowhere in the
# file and failed against a package that was perfectly correct.
grep -qE 'env[[:space:]]*=' "$pkg/mcpp.toml" || {
    cat "$pkg/mcpp.toml"
    echo "FAIL: the predicates carry no env axis, so both legs match both targets"
    exit 1; }

# ── and each consumer resolves its own ──────────────────────────────────
cd "$TMP"
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
EOF
cat > app/mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = { path = "$PKG_HOST" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

check() {   # $1 = label, $2 = expected leg dir, $3.. = build args
    local label="$1" want="$2"; shift 2
    rm -rf app/target
    ( cd app && "$MCPP" build "$@" > "$TMP/$label.log" 2>&1 ) \
        || { cat "$TMP/$label.log"; echo "$label build failed"; exit 1; }
    local nj; nj="$(find app/target -name build.ninja | head -1)"
    # Separator-agnostic, and matched from the PACKAGE name rather than from
    # `dist/`. A native mcpp.exe writes native separators into build.ninja, so
    # the first version of this — `grep -o "dist/mathkit-0.1.0/lib/…"` — matched
    # nothing at all on Windows and reported "saw more than one leg" over an
    # empty list, which reads like a packaging bug and is a grep bug.
    grep -oE "mathkit-0\.1\.0[\\/]lib[\\/][A-Za-z0-9_-]+" "$nj" \
        | sed 's|.*[\\/]||' | sort -u > "$TMP/$label.legs"
    [[ "$(wc -l < "$TMP/$label.legs")" -eq 1 ]] || {
        echo "$label saw $(wc -l < "$TMP/$label.legs") leg(s), expected exactly 1:"
        cat "$TMP/$label.legs"
        echo "--- lines mentioning the package ---"
        grep -n 'mathkit' "$nj" | head -20
        exit 1; }
    grep -qx "$want" "$TMP/$label.legs" || {
        echo "$label picked the wrong leg:"; cat "$TMP/$label.legs"; exit 1; }
}

# Native first: that is the case a bare-triple key silently failed.
check native "$MSVC_LEG"
check msvc   "$MSVC_LEG"  --target "$MSVC_LEG"
check mingw  "$MINGW_LEG" --target "$MINGW_LEG"

echo "PASS: one Windows package carries an MSVC leg and a MinGW leg, chosen apart"
