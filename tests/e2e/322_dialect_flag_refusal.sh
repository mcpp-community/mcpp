#!/usr/bin/env bash
# requires: gcc
# 322_dialect_flag_refusal.sh — a dialect flag that misses the std BMI (#527).
#
# `[build] cxxflags = ["-fno-exceptions"]` reaches every translation unit and
# not the precompiled `import std` BMI, so every importer fails inside a file
# mcpp generated, with a message that names the mechanism and not the key that
# fixes it. The build cannot succeed, which is why this is refused rather than
# warned about — contrast the host-dependence diagnostics, which fire on builds
# that work.
#
# FIVE CASES, AND THREE OF THEM MUST STAY GREEN. A check that refuses whenever
# it sees `-fno-exceptions` would pass the first two and has stopped testing
# the condition it claims to test.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p src

writes_manifest() {  # $1 = the [build] body
    printf '[package]\nname = "dia"\nversion = "0.1.0"\nstandard = "c++23"\n\n[build]\n%s\n' "$1" > mcpp.toml
}
uses_import_std() {
    printf 'import std;\nint main(){ std::println("ok"); return 0; }\n' > src/main.cpp
}
no_import_std() {
    printf '#include <cstdio>\nint main(){ std::printf("ok\\n"); return 0; }\n' > src/main.cpp
}

# ── 1. refused, and the message names the key that fixes it ─────────────────
uses_import_std
writes_manifest 'cxxflags = ["-fno-exceptions"]'
rm -rf target
if "$MCPP" build > r1.log 2>&1; then
    echo "FAIL: -fno-exceptions in cxxflags + import std should be refused"
    cat r1.log; exit 1
fi
grep -q 'dialect_cxxflags' r1.log || {
    echo "FAIL: the refusal does not name \`dialect_cxxflags\`"
    cat r1.log; exit 1
}
# Refused BEFORE compiling: a refusal that arrives after the compiler has
# already produced the confusing error has added a line, not removed one.
#
# NOT `grep 'language dialect differs'`. The refusal QUOTES that phrase, so
# the assertion matched its own message and failed on a correct implementation.
# The signal that no compile ran is the absence of run_build_plan's "Compiling"
# banner, which is printed after prepare_build returns.
grep -q '^ *Compiling' r1.log && {
    echo "FAIL: the compiler ran — the check fired too late"
    cat r1.log; exit 1
}
[ ! -d target ] || [ -z "$(find target -name '*.o' 2>/dev/null)" ] || {
    echo "FAIL: objects were produced before the refusal"
    exit 1
}

# ── 2. -fno-rtti is the same axis, so the list is exercised past entry one ──
writes_manifest 'cxxflags = ["-fno-rtti"]'
rm -rf target
if "$MCPP" build > r2.log 2>&1; then
    echo "FAIL: -fno-rtti in cxxflags + import std should be refused"; cat r2.log; exit 1
fi

# ── 3. the effective flag set, not one table ────────────────────────────────
#
# The same flag arrives from a profile and from a `[target...]` block, and both
# reach the compile line while neither reaches the prebuild. Reading only
# `[build] cxxflags` would be silent here.
printf '[package]\nname = "dia"\nversion = "0.1.0"\nstandard = "c++23"\n\n[profile.dev]\ncxxflags = ["-fno-exceptions"]\n' > mcpp.toml
rm -rf target
if "$MCPP" build > r3.log 2>&1; then
    echo "FAIL: a dialect flag in [profile.dev] cxxflags should be refused too"
    cat r3.log; exit 1
fi

# ── 4. GREEN: the correct spelling builds ───────────────────────────────────
writes_manifest 'dialect_cxxflags = ["-fno-exceptions"]'
rm -rf target
"$MCPP" build > g1.log 2>&1 || {
    echo "FAIL: dialect_cxxflags is the documented fix and must build"
    cat g1.log; exit 1
}

# ── 5. GREEN: an AUTO-PROMOTED dialect flag in cxxflags is silent ───────────
#
# `-D_GLIBCXX_USE_CXX11_ABI=` is already promoted into the prebuild by
# `dialect_flags()`, so it does reach the BMI. A check keyed on "is this flag
# dialect-class" instead of "did it reach the prebuild" would refuse here.
writes_manifest 'cxxflags = ["-D_GLIBCXX_USE_CXX11_ABI=0"]'
rm -rf target
"$MCPP" build > g2.log 2>&1 || {
    echo "FAIL: an auto-promoted dialect flag must still build"
    cat g2.log; exit 1
}

# ── 6. GREEN: no `import std` in the graph, so there is nothing to disagree ─
#
# THE DENOMINATOR.
no_import_std
writes_manifest 'cxxflags = ["-fno-exceptions"]'
rm -rf target
"$MCPP" build > g3.log 2>&1 || {
    echo "FAIL: without import std, -fno-exceptions is an ordinary per-unit flag"
    cat g3.log; exit 1
}

echo "PASS: 322_dialect_flag_refusal"
