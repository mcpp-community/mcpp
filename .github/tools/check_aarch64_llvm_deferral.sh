#!/usr/bin/env bash
# ⭐⭐ A DEFERRAL'S PREMISE, RECHECKED.
#
# `available_toolchain_indexes()` omits llvm on non-x86_64 Linux because no
# linux-aarch64 llvm exists — not in xlings-res, and not upstream since 19.x.
# That is a deferral, and a deferral nobody rechecks is indistinguishable from
# a defect.
#
# ⚠️ THIS FAILS WHEN THE REASON STOPS HOLDING, which is the opposite of what a
# check usually does. The day an aarch64 llvm is published, it goes red and
# names the gate to remove — see
# `.agents/docs/2026-08-26-aarch64-linux-ecosystem-closure.md` §P1.
#
# ⚠️ Network trouble must not be read as "it appeared". An unreadable asset list
# leaves the premise alone and says so: a check that turns a flaky API into a
# claim about the world is worse than no check.
set -uo pipefail

fail=0
for tag in 22.1.8 20.1.7; do
    names="$(curl -sSL --retry 3 --retry-all-errors --max-time 60 \
               "https://api.github.com/repos/xlings-res/llvm/releases/tags/$tag" 2>/dev/null \
             | grep -oE '"name"[[:space:]]*:[[:space:]]*"[^"]+"' \
             | sed -E 's/.*"([^"]+)"$/\1/')"
    if [ -z "$names" ]; then
        echo "  ?   $tag: could not read the asset list — premise left alone"
        continue
    fi
    if printf '%s\n' "$names" | grep -q 'linux-aarch64'; then
        echo "::error::xlings-res/llvm $tag now publishes a linux-aarch64 asset"
        echo "        the deferral in available_toolchain_indexes() has outlived its reason"
        echo "        see .agents/docs/2026-08-26-aarch64-linux-ecosystem-closure.md §P1"
        fail=1
        continue
    fi
    echo "  ok  $tag: still no linux-aarch64 asset"
done
[ "$fail" = 0 ] || exit 1
echo "OK: the aarch64 llvm deferral still has its reason"
