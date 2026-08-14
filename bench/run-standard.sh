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
#     bash bench/run-standard.sh --resume        # continue an interrupted run
#
# ── RESUMING ───────────────────────────────────────────────────────────────
#
# mbench records every measured unit to `.mbench/<fingerprint>/journal.jsonl` as
# it goes, so an interrupted run costs at most the unit in flight. `--resume`
# re-runs this script against that cache: units already recorded are replayed
# from it, the rest are measured.
#
# It is a FLAG rather than the default because the two behaviours protect
# against opposite mistakes. Without it a second invocation must never write
# beside the first one's reports (see the supersede block below — two runs did
# end up spliced in one directory). With it, that is precisely what is wanted.
# The fingerprint is what makes it safe: it covers the whole configuration, so a
# resume that is not actually the same run lands in a different cache and starts
# from zero on its own.
#
# ⚠️ SEED BUILDS ARE NOT UNITS AND ARE REDONE. An incremental scenario needs a
# tree that is already up to date, and that state was built by a seed build that
# no journal can hold. A resumed cell therefore pays its seed again before it
# can skip anything — resume is cheap, not free.
set -uo pipefail

# BENCH_ROOT lets this be run from a COPY. Editing a bash script while it is
# executing corrupts the running instance — bash reads the file incrementally,
# so an edit shifts byte offsets under it, and one run ended in
#     line 193: ather: command not found
#     line 199: tc: unbound variable
# That happened twice. Copy the script somewhere, point BENCH_ROOT at the
# repository, and the original can be edited freely while it runs.
ROOT="${BENCH_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
MATRIX="$ROOT/bench/matrix.json"
RUNS=3
DRY=0
RESUME=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1 ;;
        --resume)  RESUME=1 ;;
        --runs)    RUNS="${2:?--runs needs a number}"; shift ;;
        -h|--help) sed -n '2,55p' "${BASH_SOURCE[0]}"; exit 0 ;;
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

BENCH="$(ls -t "$ROOT"/bench/target/*/*/bin/mbench 2>/dev/null | head -1)"
[ -n "$BENCH" ] || say_missing "the harness — run: (cd bench && mcpp build --release)"

MCPP_BIN="$(ls -t "$ROOT"/target/*/*/bin/mcpp 2>/dev/null | head -1)"
[ -n "$MCPP_BIN" ] || say_missing "the mcpp under test — run: mcpp build --release"

# ⚠️ THE ENGINE MUST BE A BINARY, NEVER THE BARE NAME `mcpp`.
#
# A bare `mcpp` resolves through PATH to the xlings shim, and that shim RE-PICKS
# its version from the working directory — which, for a `--project` run, is the
# measured tree. Every pinned workload carries its own pin, so the whole first
# run of this script measured mcpp@2026.8.11.3 in every cell: the released
# binary, not the branch, and with no old-vs-new column at all. The tell was in
# the report the entire time — `mcpp@2026.8.11.3` on rows that were supposed to
# be the build under test — and `touch-hub 76.55s` on mcpp's own tree, which is
# the old binary's byte-comparison behaviour, not this branch's.
#
# So `mcpp` in a cell's engine list is expanded here into explicit paths: the
# build under test, plus the released reference when it can be resolved AND
# asserts its own version. xlings unpacks each version to
# data/runtimedir/mcpp-<ver>-<os>-<arch>/mcpp.
REFERENCE_MCPP="$(python3 -c "import json;print(json.load(open('$MATRIX',encoding='utf-8')).get('reference_mcpp',''))" 2>/dev/null)"
REF_BIN=""
if [ -n "$REFERENCE_MCPP" ]; then
    for c in "$HOME"/.xlings/data/runtimedir/mcpp-"$REFERENCE_MCPP"-*/mcpp; do
        [ -x "$c" ] || continue
        got="$("$c" --version 2>/dev/null | grep -oE '[0-9]+(\.[0-9]+){2,3}' | head -1)"
        if [ "$got" = "$REFERENCE_MCPP" ]; then REF_BIN="$c"; break; fi
        echo "  note: $c reports '$got', not '$REFERENCE_MCPP'; ignoring it"
    done
    [ -n "$REF_BIN" ] || echo "  note: no mcpp@$REFERENCE_MCPP binary found; the old-vs-new column will be missing"
fi

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
# ⚠️ REFUSE TO MIX RUNS. A previous invocation's reports must not sit beside
# this one's under the same names.
#
# It happened: an earlier run was stopped with SIGTERM, did not die immediately,
# finished the cell it was on and wrote its report AFTER the `rm -rf` that was
# meant to clear the directory — so a 90-cell file from one run sat next to a
# 72-cell file from another, and the only way to tell was to read `started_at`
# out of the JSON. A table generated from that directory would have spliced two
# runs silently, which is the failure this suite exists to remove.
#
# Same shape as the timeout leak the harness itself had: a process outliving the
# command that was supposed to end it.
# One cache root for the whole set, pinned to the repository rather than to the
# working directory: resume must not depend on where the script was invoked from.
CACHE="$ROOT/.mbench"

if [ "$RESUME" = 1 ]; then
    if [ -d "$CACHE" ]; then
        echo "resuming: $(find "$CACHE" -name journal.jsonl -exec cat {} + 2>/dev/null | wc -l)"\
             "unit(s) already recorded under ${CACHE#"$ROOT"/}"
    else
        echo "note: --resume given but ${CACHE#"$ROOT"/} does not exist; this run starts from zero."
    fi
    echo
elif [ -d "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then
    stamped="$OUT.superseded-$(date -u +%H%M%S)"
    mv "$OUT" "$stamped"
    echo "note: $(basename "$OUT") already held files from an earlier run;"
    echo "      moved to $(basename "$stamped") rather than writing beside them."
    echo
fi

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
    # Expand the bare `mcpp` into explicit binaries — see REFERENCE_MCPP above.
    spec=""
    for e in ${engines//,/ }; do
        case "$e" in
            # Bare `mcpp` is the old-vs-new pair: the build under test, plus the
            # released reference when one could be resolved.
            mcpp) spec="${spec:+$spec,}mcpp=$MCPP_BIN"
                  [ -n "$REF_BIN" ] && spec="$spec,mcpp=$REF_BIN" ;;
            # `mcpp[...]` is an OPT-IN ARM of the build under test, and only of
            # it. Running the reference with `schedule=on` would answer a
            # question nobody asked — the released binary predates the fix in
            # §8b, so the arm would measure a known-broken scheduler and the
            # column would read as a regression in the feature.
            "mcpp["*"]") spec="${spec:+$spec,}${e}=$MCPP_BIN" ;;
            *)    spec="${spec:+$spec,}$e" ;;
        esac
    done

    args=(--engines "$spec" --variants "$variants" --scenarios "$scenarios"
          --baseline "$baseline" --profile release --runs "$RUNS" --timeout 1800
          --work "$WORK" --out "$OUT/$tc-$proj.json" --cache-root "$CACHE")

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
