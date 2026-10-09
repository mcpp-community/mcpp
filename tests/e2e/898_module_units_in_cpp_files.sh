#!/usr/bin/env bash
# requires: gcc llvm
# 898 -- the module declaration decides a unit's role, the extension its
# language (D8, design 2026-10-10 §5; #790).
#
# The rule used to be chosen by extension: a `.cpp` holding a module unit was
# compiled as a plain object. Under Clang that meant
#
#   M1  `module m:part;` in a .cpp declared a BMI its edge never wrote, so
#       every build compiled it again and relinked (R4);
#   M2  `export module m;` in a .cpp wrote no BMI, and the importer failed
#       with `module 'm' not found` (R5);
#   M3  `import :part;` of such a partition failed the same way (R6).
#
# GCC reads the role from the content and built all three; it is the control.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

steps() { grep -cE '^\[[0-9]+/' "$1" || true; }

for tc in llvm@23.1.3 gcc@16.1.0; do
    # ── M1, M3 ────────────────────────────────────────────────────────────
    P="$TMP/$tc/parts"; mkdir -p "$P/src"; cd "$P"
    printf '[package]\nname = "repro"\nversion = "0.1.0"\n[toolchain]\ndefault = "%s"\n[targets.repro]\nkind = "bin"\nmain = "src/main.cpp"\n' "$tc" > mcpp.toml
    printf 'export module repro;\nexport import :api;\nimport :api_impl;\n' > src/repro.cppm
    printf 'export module repro:api;\nexport auto answer() -> int;\n' > src/api.cppm
    printf 'module repro:api_impl;\nimport :api;\nauto answer() -> int { return 42; }\n' > src/api_impl.cpp
    printf 'import repro;\nimport std;\nauto main() -> int { std::println("{}", answer()); }\n' > src/main.cpp
    "$MCPP" build > b1.log 2>&1 || fail "$tc: a .cpp implementation partition, imported" b1.log
    "$MCPP" build --verbose > b2.log 2>&1 || fail "$tc: second build" b2.log
    [ "$(steps b2.log)" = 0 ] || fail "$tc: the second build did $(steps b2.log) steps" b2.log
    [ "$(find target -path '*/bin/repro*' -type f -exec {} \; | tr -d '\r')" = 42 ] || fail "$tc: wrong answer" b1.log

    # ── M2 ────────────────────────────────────────────────────────────────
    I="$TMP/$tc/iface"; mkdir -p "$I/src"; cd "$I"
    printf '[package]\nname = "r5"\nversion = "0.1.0"\n[toolchain]\ndefault = "%s"\n[targets.r5]\nkind = "bin"\nmain = "src/main.cpp"\n' "$tc" > mcpp.toml
    printf 'export module r5;\nexport auto answer() -> int { return 42; }\n' > src/r5.cpp
    printf 'import r5;\nimport std;\nauto main() -> int { std::println("{}", answer()); }\n' > src/main.cpp
    "$MCPP" build > b1.log 2>&1 || fail "$tc: an interface unit in a .cpp" b1.log
    "$MCPP" build --verbose > b2.log 2>&1 || fail "$tc: second build" b2.log
    [ "$(steps b2.log)" = 0 ] || fail "$tc: the second build did $(steps b2.log) steps" b2.log
    echo "ok $tc"
done

echo "PASS: 898_module_units_in_cpp_files"
