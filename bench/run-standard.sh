#!/usr/bin/env bash
# bench/run-standard.sh — produce the STANDARD DATA SET on this machine.
#
# One command, no arguments. It replaces the CI matrix that used to live in
# .github/workflows/bench.yml, and it exists because that matrix was measuring
# far less than it appeared to:
#
#     10 cells, 32 foreign-engine arms, 12 of them (37%) waived by
#     `allow_failed` — xmake had MORE arms waived (6) than measured (4)
#
# A job that goes green while a third of the comparison never ran is the exact
# failure this suite was built to remove, so it may not be how the suite runs.
# Two structural reasons on top of that: a shared runner measures the RUNNER
# (the same tree is 243s there and 79s on a developer box), and the gaps being
# waived are other people's tools, not mcpp.
#
# ── THE STANDARD SET IS DERIVED, NOT A SECOND LIST ─────────────────────────
#
# bench/matrix.json stays the single place the matrix is written down. This
# script does not repeat it; it selects the cells for THIS os and runs every
# engine they list.
#
# ⚠️ `allow_failed` IS DELIBERATELY IGNORED HERE, and the first version of this
# script got that wrong. Those waivers were recorded against failures on the CI
# RUNNER — cmake's `__CMAKE::CXX23` on the mcpp tree, cmake's `manifest has no
# sources` on the xlings tree — and both of those arms configure and generate
# perfectly well on a developer machine; they were verified doing so. Filtering
# by them would have carried a runner's limitation into local data and quietly
# published a smaller comparison than this machine can actually make.
#
# So nothing is pre-excluded and no `--allow-failed` is passed. An arm that
# cannot run here FAILS here, loudly, and that is data: a gap is only real once
# it has been reproduced on the machine making the claim.
#
# Usage:
#     bash bench/run-standard.sh                 # everything for this OS
#     bash bench/run-standard.sh --dry-run       # print the plan and stop
#     bash bench/run-standard.sh --runs 1        # faster, NOT publishable
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MATRIX="$ROOT/bench/matrix.json"
RUNS=3
DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1 ;;
        --runs)    RUNS="${2:?--runs needs a number}"; shift ;;
        -h|--help) sed -n '2,32p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

[ -f "$MATRIX" ] || { echo "no bench/matrix.json at $MATRIX" >&2; exit 1; }

case "$(uname -s)" in
    Linux)  OS=linux  ;;
    Darwin) OS=macos  ;;
    MINGW*|MSYS*|CYGWIN*) OS=windows ;;
    *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac
ARCH="$(uname -m)"

# ── Preconditions, each named rather than discovered three steps later ──────
#
# Every one of these has cost a full run before: a missing submodule makes the
# `hub` file absent and every perturbation scenario skip; an unpinned tool makes
# the numbers describe a different program; a dependency that was served from
# cache without being unpacked makes the cmake arm compile three units short.
fail=0
say_missing() { echo "  MISSING: $*"; fail=1; }

BENCH="$(ls -t "$ROOT"/bench/target/*/*/bin/bench 2>/dev/null | head -1)"
[ -n "$BENCH" ] || say_missing "the harness — run: (cd bench && mcpp build --release)"

MCPP_BIN="$(ls -t "$ROOT"/target/*/*/bin/mcpp 2>/dev/null | head -1)"
[ -n "$MCPP_BIN" ] || say_missing "the mcpp under test — run: mcpp build --release"

for t in cmake xmake bazel; do
    want="$(python3 -c "import json;print(json.load(open('$MATRIX',encoding='utf-8'))['tools'].get('$t',''))" 2>/dev/null)"
    have="$(command -v "$t" >/dev/null 2>&1 && "$t" --version 2>/dev/null | head -1 || true)"
    if [ -z "$have" ]; then
        echo "  note: $t is not installed; its arms will report unavailable (pinned: $want)"
    elif [ -n "$want" ] && ! printf '%s' "$have" | grep -q "$want"; then
        echo "  ⚠ $t is $have but matrix.json pins $want — the numbers would describe another tool"
        fail=1
    fi
done

# The pinned workloads. A submodule that is present-but-empty is the shape that
# reads as "checked out" to a path test and produces an empty measurement.
while read -r proj; do
    [ -n "$proj" ] || continue
    d="$ROOT/bench/projects"
    case "$proj" in
        mcpp-*)   d="$d/mcpp/$proj" ;;
        xlings-*) d="$d/xlings/$proj" ;;
        *) continue ;;
    esac
    if [ ! -d "$d" ] || [ -z "$(ls -A "$d" 2>/dev/null)" ]; then
        say_missing "workload $proj at $d — run: git submodule update --init"
    fi
done < <(python3 -c "
import json
d = json.load(open('$MATRIX', encoding='utf-8'))
print('\n'.join(sorted({c['project'] for c in d['cells'] if c['project'] != 'fixture'})))
" 2>/dev/null)

[ "$fail" = 0 ] || { echo; echo "preconditions not met; nothing was run." >&2; exit 1; }

# ── The plan: this OS's cells, minus every waived arm ───────────────────────
PLAN="$(python3 - "$MATRIX" "$OS" <<'PY'
import json, sys
m = json.load(open(sys.argv[1], encoding="utf-8"))
os_want = sys.argv[2]
for c in m["cells"]:
    if c.get("os") != os_want:
        continue
    # Every engine the cell lists. `allow_failed` is a CI-era record of what
    # broke on a shared runner and says nothing about this machine.
    engines = [e.strip() for e in c["engines"].split(",") if e.strip()]
    if not engines:
        continue
    # ⚠️ NOT A TAB. `IFS=$'\t' read` treats tab as IFS WHITESPACE, so consecutive
    # tabs COLLAPSE — a cell with no `leaf` shifted every later field one place
    # left and cmake was handed the BASELINE as its source directory:
    #     CMake Error: The source directory ".../bench/projects/2026.8.11.3"
    #     does not exist.
    # Five cmake cells failed that way, and the message names neither the field
    # that was wrong nor the empty one before it. \x1f is not whitespace, so an
    # empty field survives as an empty field.
    print("\x1f".join([
        c["toolchain"], c["project"], ",".join(engines), c["variants"], c["scenarios"],
        c.get("hub", ""), c.get("body", ""), c.get("leaf", ""),
        c.get("buildfiles", c["project"]), c.get("baseline", m["baseline"]),
        c.get("preset", "standard"),
    ]))
PY
)"

[ -n "$PLAN" ] || { echo "no cells for os=$OS in matrix.json" >&2; exit 1; }

STAMP="$(date -u +%Y%m%d)"
OUT="$ROOT/bench/results/standard-$STAMP-$OS-$ARCH"
# ⚠️ SCRATCH DOES NOT LIVE IN results/. Putting the work tree under $OUT
# meant engine scratch — including JSON files whose top level is an ARRAY —
# landed in the directory the README guard scans for published medians, and
# it died with `AttributeError: 'list' object has no attribute 'get'`.
# It would also have been committed along with the data.
WORK="${TMPDIR:-/tmp}/bench-standard-$STAMP-$$"
echo "standard set: $(printf '%s\n' "$PLAN" | wc -l) cells, ${RUNS} run(s) each, os=$OS arch=$ARCH"
echo "output       : ${OUT#"$ROOT"/}"
echo

# ⚠️ NOT `printf ... | while`. A pipeline runs its right-hand side in a SUBSHELL,
# so a failure counter incremented inside the loop does not survive it and the
# script exits 0 no matter what happened. Verified with a stub engine that exits
# 7 for every cell: the old shape printed seven failures and then exited 0 — and
# went on to print "how to tell whether this data is publishable: 1. every cell
# exited 0", handing the reader a question it had the answer to.
#
# That is the exact defect this suite exists to remove, in the script written to
# replace a CI that had it. Process substitution keeps the loop in THIS shell.
FAILED=0
while IFS=$'\x1f' read -r tc proj engines variants scenarios hub body leaf buildfiles baseline preset; do
    echo "  $tc/$proj  [$engines]"
    [ "$DRY" = 1 ] && continue

    mkdir -p "$OUT"
    args=(--engines "$engines" --variants "$variants" --scenarios "$scenarios"
          --baseline "$baseline" --profile release --runs "$RUNS" --timeout 1800
          --work "$WORK" --out "$OUT/$tc-$proj.json")

    # `payload:gcc` / `payload:clang` resolve to the hermetic driver every engine
    # is handed — never `command -v g++`, which inside an xlings workspace is a
    # shim whose include path moves with the workspace.
    case "$tc" in
        gcc)   args+=(--compiler payload:gcc) ;;
        clang) args+=(--compiler payload:clang) ;;
        msvc)  ;;                       # a label, not a path
    esac

    if [ "$proj" = "fixture" ]; then
        args+=(--preset "$preset")
    else
        case "$proj" in
            mcpp-*)   pdir="$ROOT/bench/projects/mcpp/$proj" ;;
            xlings-*) pdir="$ROOT/bench/projects/xlings/$proj" ;;
        esac
        args+=(--project "$pdir" --buildfiles "$ROOT/bench/projects/$buildfiles")
        [ -n "$hub" ]  && args+=(--hub  "$hub")
        [ -n "$body" ] && args+=(--body "$body")
        [ -n "$leaf" ] && args+=(--leaf "$leaf")
    fi

    # `mcpp` is also passed as an explicit engine binary so the report labels it
    # with the version that binary reports, rather than whatever a PATH shim
    # resolves to from the measured tree's directory.
    if [ "${BENCH_PRINT_ARGV:-0}" = 1 ]; then
        printf '    argv:'; printf ' %q' "${args[@]}"; printf '\n'
        continue
    fi
    "$BENCH" "${args[@]}" 2>&1 | tee "$OUT/$tc-$proj.log" | sed 's/^/    /'
    rc=${PIPESTATUS[0]}
    if [ "$rc" != 0 ]; then
        echo "    ^ cell exited $rc — see ${OUT#"$ROOT"/}/$tc-$proj.log"
        FAILED=$((FAILED + 1))
    fi
done < <(printf '%s\n' "$PLAN")

[ "$DRY" = 1 ] && exit 0

echo
if [ "$FAILED" != 0 ]; then
    echo "── NOT PUBLISHABLE ─────────────────────────────────────────────────────"
    echo "  $FAILED cell(s) failed. Nothing here is pre-excluded, so each one is a"
    echo "  real failure on THIS machine: reproduce it by hand before calling it a"
    echo "  gap, and do not publish a table that quietly omits it."
    echo
    echo "reports: ${OUT#"$ROOT"/}"
    exit 1
fi

echo "── the remaining checks, which this script cannot make for you ──────────"
echo "  2. no cell reports \`failed\` — nothing is pre-excluded, so a failure is real"
echo "     (if one is, reproduce it by hand before calling it a gap)"
echo "  3. no \`cold\` tripped an invariant (vs its own noop, vs its peers)"
echo "  4. min/max within about ±20% of the median; wider means this machine was noisy"
echo
echo "reports: ${OUT#"$ROOT"/}"
