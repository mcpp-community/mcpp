#!/usr/bin/env bash
#
# Guard: no function under the prepare.cppm decomposition grows past ~400
# lines (mcpp-community/mcpp#722, T6 of the 2026-09-27 round).
#
# WHY
#
# check_file_lengths.sh caps each FILE at 2,500 lines. It says nothing about
# a single FUNCTION inside a file that stays under the cap while one phase
# function alone climbs back past a thousand lines and closes back over the
# ~180-local shape prepare.cppm was split to remove in the first place (see
# that script's own header, and the layout comment atop src/build/prepare.cppm).
# #722 split the seven functions that had grown past ~400 lines into
# sub-steps named after the sections their own banners already used; this
# gate is what keeps a phase function from quietly growing back into one.
#
# THE RULE
#
# Every function defined in a file directly under src/build/prepare/ (or in
# src/build/prepare.cppm itself) stays at or under LINE_THRESHOLD lines, as
# clang-tidy's readability-function-size check counts them (its own count,
# not a text-heuristic line counter -- a brace-counting or regex-based
# stand-in cannot tell a function's extent from a `{`/`}` pair inside a
# string literal or a designated initializer, both common in this codebase's
# std::format calls and manifest structs; see .agents/docs/
# 2026-09-27-eight-reports-by-home-and-one-optimisation-plan.md §8).
#
# WHAT THIS NEEDS
#
# A compile database that names BMIs explicitly (-fmodule-file=...), which
# only a build actually produces: `mcpp build --toolchain llvm@23.1.3` writes
# compile_commands.json at the project root. This script does not build it:
# the caller runs that build first. check_file_lengths.sh needs no such
# division because it reads the tree.
#
# IN CI SINCE 2026.9.28.2 (#729). ci-linux.yml's "toolchain: musl + llvm" job
# builds mcpp with llvm@23.1.3 -- failing on the build's own status, which it
# did not do while it built with llvm@20.1.7 and read only the resolution line
# -- and runs this script after it, over the compile database that build
# writes. By hand: `mcpp build --toolchain llvm@23.1.3`, then this script.
#
# clang-tidy itself is not part of the plain xim:llvm payload mcpp resolves
# for `--toolchain llvm@...` (measured: xim-x-llvm/22.1.8/bin has clang,
# clang-scan-deps and the LLVM binutils, no clang-tidy). It ships in the
# sibling package `xim:llvm-tools` at the same version -- resolved and
# searched for under the xlings package store; install it with
# `xlings install xim:llvm-tools@<version that matches your llvm toolchain>`
# if this script cannot find it.
#
# Usage: bash .github/tools/check_function_sizes.sh [repo_dir]

set -uo pipefail

REPO_DIR="${1:-$(pwd)}"
cd "$REPO_DIR" || { echo "FAIL: cannot cd to $REPO_DIR" >&2; exit 1; }

LINE_THRESHOLD=400
DIR="src/build/prepare"
PRIMARY="src/build/prepare.cppm"
CDB="compile_commands.json"

[ -d "$DIR" ] || { echo "FAIL: $DIR does not exist -- this guard has gone stale" >&2; exit 1; }

if [ ! -f "$CDB" ]; then
    cat >&2 <<EOF
FAIL: $CDB does not exist.
  This check reads clang-tidy's own function boundaries, which needs a
  compile database that names every imported module's BMI explicitly.
  Produce one first:
      mcpp build --toolchain llvm@23.1.3
  (any installed LLVM row works; the database is written at the project
  root regardless of the row's exact version).
EOF
    exit 1
fi

# Locate clang-tidy. It is not in the plain xim:llvm payload (see the header
# comment); it is the sibling xim:llvm-tools payload, and it must be the
# version of the clang that wrote compile_commands.json, because it reads the
# BMIs that clang wrote. `CLANG_TIDY` may name it explicitly; otherwise
# the version is read from the compiler path the database names
# (`.../xim-x-llvm/<version>/bin/clang++`) and looked up in either xlings store.
cdb_llvm_version() {
    grep -o 'xim-x-llvm/[0-9][0-9.]*/bin/clang' "$CDB" 2>/dev/null | head -1 \
        | sed 's|xim-x-llvm/\([0-9.]*\)/bin/clang|\1|'
}
find_clang_tidy() {   # $1 = the llvm version
    local root
    for root in "${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs" "$HOME/.xlings/data/xpkgs"; do
        [ -x "$root/xim-x-llvm-tools/$1/bin/clang-tidy" ] \
            && { echo "$root/xim-x-llvm-tools/$1/bin/clang-tidy"; return 0; }
    done
    return 1
}

if [ -n "${CLANG_TIDY:-}" ]; then
    [ -x "$CLANG_TIDY" ] || { echo "FAIL: CLANG_TIDY=$CLANG_TIDY is not executable" >&2; exit 1; }
else
    LLVM_VERSION="$(cdb_llvm_version)"
    CLANG_TIDY="$( [ -n "$LLVM_VERSION" ] && find_clang_tidy "$LLVM_VERSION" )" || {
        cat >&2 <<EOF
FAIL: no clang-tidy of the llvm version that wrote $CDB (${LLVM_VERSION:-unknown})
      was found under an xlings package store. Install that toolchain's sibling:
          xlings install xim:llvm-tools@${LLVM_VERSION:-<version>}
EOF
        exit 1
    }
fi

# The files this database actually has entries for, restricted to the
# decomposition's own directory (plus the primary interface, if it is ever
# given its own compiled entry point -- it has none today, since it defines
# only declarations and inline exports; the loop below tolerates that).
mapfile -t FILES < <(python3 - "$CDB" "$DIR" "$PRIMARY" <<'PYEOF'
import json, sys
cdb_path, dirname, primary = sys.argv[1], sys.argv[2], sys.argv[3]
with open(cdb_path) as f:
    entries = json.load(f)
seen = set()
for e in entries:
    path = e["file"]
    if f"/{dirname}/" in path or path.endswith(f"/{primary}"):
        seen.add(path)
for p in sorted(seen):
    print(p)
PYEOF
)

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "FAIL: $CDB has no entry under $DIR -- was it built with a matching source tree?" >&2
    exit 1
fi

echo "checking ${#FILES[@]} file(s) with $CLANG_TIDY (LineThreshold=$LINE_THRESHOLD)..."

OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT

# No --warnings-as-errors: the only check enabled is readability-function-size
# itself, and a finding in json.hpp (bundled third-party, reached through one
# of these files' imports) would then make clang-tidy exit non-zero on every
# run regardless of this decomposition's own state -- exactly the ambiguity
# the "diagnostic tool problem" branch below exists to catch, and it cannot
# tell the two apart from an exit code alone. The `relevant` filter is the
# sole pass/fail signal; clang-tidy's own exit code is read only as a sign
# that the tool itself failed to run (a bad compile command, a crash), which
# a plain warning never produces.
"$CLANG_TIDY" \
    --checks='-*,readability-function-size' \
    --config="{CheckOptions: {readability-function-size.LineThreshold: '$LINE_THRESHOLD'}}" \
    -p "$REPO_DIR" \
    "${FILES[@]}" > "$OUT" 2>&1
rc=$?

# Only findings inside the decomposition's own directory gate the build: a
# bundled third-party header (e.g. modules/libs/src/json/json.hpp) reached
# through one of these files' imports is not this decomposition's to fix.
# A finding is a diagnostic line, which ends with the bracketed check name; a
# crash dump also names the check (in its program arguments) together with
# every file path, and must not read as a finding.
relevant=$(grep -E '\[readability-function-size\]$' "$OUT" | grep -F -e "/$DIR/" -e "/$(basename "$PRIMARY")" || true)

if [ -n "$relevant" ]; then
    echo "$relevant" >&2
    echo >&2
    echo "FAIL: function(s) over $LINE_THRESHOLD lines under $DIR -- see above." >&2
    echo "  Split at the sub-section boundaries its own banners already name" >&2
    echo "  (mcpp-community/mcpp#722's own method), the way phase13_finish," >&2
    echo "  phase4b_graph_worklist, phase6_features_and_host_tools and" >&2
    echo "  phase9_target_side were split." >&2
    exit 1
fi

if [ "$rc" -ne 0 ]; then
    echo "FAIL: clang-tidy exited $rc with no readability-function-size finding under $DIR" >&2
    echo "  (a diagnostic tool problem, not a function-size one -- see the log):" >&2
    cat "$OUT" >&2
    exit 1
fi

echo "ok: no function under $DIR (or $PRIMARY) exceeds $LINE_THRESHOLD lines"
exit 0
