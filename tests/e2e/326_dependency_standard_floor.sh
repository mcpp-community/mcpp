#!/usr/bin/env bash
# requires: gcc
# 326_dependency_standard_floor.sh — a dependency that DECLARED a higher
# standard than the graph is built at (#527 RFC 3).
#
# A C++ module graph has one standard: cross-level BMIs are hard incompatible,
# so the root's level is imposed graph-wide and a dependency's `standard` is
# parsed and then discarded. That is correct. The defect was the silence — a
# package declaring c++26 because it needs c++26 is compiled at whatever the
# consumer says, and fails, if it fails at all, inside a translation unit the
# user does not own.
#
# SCOPED TO AUTHOR-OWNED MANIFESTS, AND THE NEGATIVE IS THE HARD HALF.
# `standardDeclared` says the key was present, not that a human meant it: every
# index descriptor with an mcpp segment declares `language` (measured: 782 of
# 782 in the local registry, 756 of those 774 packages being C libraries with
# `import_std = false` carrying a boilerplate "c++23"). A check that trusted
# declaredness everywhere would fire against the whole index for any root at
# c++20 — which is exactly why the cpp20 design doc deferred it.
#
# DEGRADED, NOT AN ERROR: a package declaring c++26 compiles fine at c++23
# whenever it does not use a C++26 construct, and that is a green build today.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p lib/src app/src
printf '[package]\nname = "flib"\nversion = "0.1.0"\nstandard = "c++26"\n[targets.flib]\nkind = "lib"\n' > lib/mcpp.toml
printf 'export module flib;\nexport int f(){ return 1; }\n' > lib/src/flib.cppm
printf '[package]\nname = "fapp"\nversion = "0.1.0"\nstandard = "c++23"\n[dependencies]\nflib = { path = "../lib" }\n' > app/mcpp.toml
printf 'import flib;\nint main(){ return f()==1 ? 0 : 1; }\n' > app/src/main.cpp

cd app

# ── it fires, and the build still succeeds ──────────────────────────────────
"$MCPP" build > warn.log 2>&1 || {
    echo "FAIL: the floor check must be degraded, not an error"
    cat warn.log; exit 1
}
grep -q 'flib' warn.log && grep -q 'c++26' warn.log || {
    echo "FAIL: the diagnostic does not name the package and the level"
    cat warn.log; exit 1
}
grep -q 'workspace.package' warn.log || {
    echo "FAIL: the hint does not name the one-place fix"
    cat warn.log; exit 1
}

# ── --strict promotes it, in the one place that policy lives ────────────────
rm -rf target
if "$MCPP" build --strict > strict.log 2>&1; then
    echo "FAIL: --strict did not promote the degradation"
    cat strict.log; exit 1
fi

# ── SILENT when the dependency did not declare one ──────────────────────────
#
# THE DENOMINATOR for "declared". Same graph, same levels, the key removed.
printf '[package]\nname = "flib"\nversion = "0.1.0"\n[targets.flib]\nkind = "lib"\n' > ../lib/mcpp.toml
rm -rf target
"$MCPP" build > silent.log 2>&1 || { cat silent.log; exit 1; }
grep -q 'declares standard' silent.log && {
    echo "FAIL: fired for a dependency that declared nothing"
    cat silent.log; exit 1
}

# ── SILENT when the dependency declares a level at or below the graph's ─────
printf '[package]\nname = "flib"\nversion = "0.1.0"\nstandard = "c++23"\n[targets.flib]\nkind = "lib"\n' > ../lib/mcpp.toml
rm -rf target
"$MCPP" build > equal.log 2>&1 || { cat equal.log; exit 1; }
grep -q 'declares standard' equal.log && {
    echo "FAIL: fired for a dependency at the graph's own level"
    cat equal.log; exit 1
}

# ── and raising the consumer silences it, which is what the hint promises ───
printf '[package]\nname = "flib"\nversion = "0.1.0"\nstandard = "c++26"\n[targets.flib]\nkind = "lib"\n' > ../lib/mcpp.toml
printf '[package]\nname = "fapp"\nversion = "0.1.0"\nstandard = "c++26"\n[dependencies]\nflib = { path = "../lib" }\n' > mcpp.toml
rm -rf target
"$MCPP" build > raised.log 2>&1 || { cat raised.log; exit 1; }
grep -q 'declares standard' raised.log && {
    echo "FAIL: the documented fix does not silence the diagnostic"
    cat raised.log; exit 1
}

echo "PASS: 326_dependency_standard_floor"
