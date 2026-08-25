#!/usr/bin/env bash
# requires: llvm unix-shell
# `toolchain list` names the targets this host can build for, including the ones
# whose system has to come from a dependency graph.
#
# ⚠️ IT USED TO FILTER ON `host_can_serve`, WHICH ANSWERS A NARROWER QUESTION —
# "can a payload here serve this target" — and presented it as "can this be
# built". Measured 2026-08-25 on Linux: `x86_64-windows-musl` was absent from
# the list while the same machine produced a real artefact for it,
#
#     $ mcpp build --target x86_64-windows-musl
#         c-abi  musl  (openkal-musl@0.3.5, graph)
#     $ file …/winmusl.exe
#       PE32+ executable (console) x86-64, for MS Windows, 14 sections
#
# because its system came from the graph.
#
# ⭐⭐ AND THE SECOND HALF IS THE POINT. Listing every vocabulary row would also
# make the first half pass, and it would tell a Linux user they can build
# `x86_64-windows-msvc` — which needs MSVC, or `aarch64-macos`, which needs the
# macOS SDK. Neither is something a dependency can supply. A list that
# over-promises is worse than one that under-promises, because the first costs
# a build to discover.
set -e

MCPP="${MCPP:-mcpp}"
out="$("$MCPP" toolchain list 2>&1 || true)"
targets="$(printf '%s\n' "$out" | awk '/^Targets:/,0')"

if [ -z "$targets" ]; then
    echo "SKIP: this build printed no Targets section"
    exit 0
fi

row() { printf '%s\n' "$targets" | grep -E "^\s+\*?\s*$1(\s|$)" | head -1; }

# ── Half one: a graph-served target is listed, and says so ────────────────
#
# Linux only: on Windows the same row IS payload-served, and on macOS there is
# no Windows-PE payload of any kind — the row's absence there is correct and
# means something else.
case "$(uname -s)" in
  Linux) ;;
  *) echo "SKIP: the graph-served row under test is a Linux-host arrangement"; exit 0 ;;
esac

wm="$(row x86_64-windows-musl)"
if [ -z "$wm" ]; then
    echo "FAIL: x86_64-windows-musl is absent, and this host can build it"
    # ⚠️ WHICH BINARY ANSWERED. A list missing a row and a list produced by an
    # older mcpp look identical, and the first CI failure of this test could not
    # tell them apart — so the evidence names the program as well as its output.
    echo "        asked: ${MCPP} ($("$MCPP" --version 2>&1 | head -1))"
    printf '%s\n' "$targets" | sed 's/^/        /'
    exit 1
fi
case "$wm" in
  *"dependency graph"*)
    echo "  ok  x86_64-windows-musl is listed as needing a dependency graph" ;;
  *installed*|*available*)
    echo "  ok  x86_64-windows-musl is listed (payload-served on this host)" ;;
  *)
    echo "FAIL: listed with a status that says neither"
    echo "        $wm"
    exit 1 ;;
esac

# ── Half two: what a graph cannot supply stays out ────────────────────────
#
# ⚠️ ASSERTS THE ROW IS ABSENT, so it first establishes that rows are being
# printed at all — an empty section would pass this trivially.
n="$(printf '%s\n' "$targets" | grep -cE '^\s+\*?\s*[a-z0-9_]+-' || true)"
if [ "${n:-0}" -lt 3 ]; then
    echo "SKIP: only ${n:-0} target rows printed — too few to assert an absence"
    exit 0
fi
echo "  ok  $n target rows printed, so an absence below means something"

fail=0
for t in x86_64-windows-msvc aarch64-macos; do
    r="$(row "$t")"
    if [ -n "$r" ]; then
        echo "FAIL: $t is offered on a Linux host, and no dependency supplies it"
        echo "        $r"
        fail=1
    else
        echo "  ok    $t stays out — it needs a host-only toolchain"
    fi
done
[ "$fail" = 0 ] || exit 1

echo "OK: the list answers what can be built, not what has a payload"
