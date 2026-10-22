#!/usr/bin/env bash
# check_release_paths.sh — a released mcpp names no directory of the machine
# that built it (mcpp#832; design 2026-10-23 §15.3, R4).
#
# mcpp's release profile trims the source paths an artifact carries
# (`trim_paths`), so `__FILE__` in an `assert` of a vendored header reads
# `libs@0.1.0/src/json/json.hpp` rather than
# `/home/runner/work/mcpp/mcpp/modules/libs/src/json/json.hpp`. This gate reads
# the shipped binary and fails on the build machine's checkout -- the source
# paths #832 found. It names exactly that directory: the binary legitimately
# carries `/tmp/` and `/home/` as string literals of its own code, and a
# dynamically linked build's loader and run path name the toolchain store,
# which is what `mcpp pack` rewrites, not a source path.
#
# Usage: bash .github/tools/check_release_paths.sh <binary>
set -uo pipefail
bin="${1:?usage: check_release_paths.sh <binary>}"
[[ -f "$bin" ]] || { echo "FAIL: no binary at $bin"; exit 1; }
roots=()
[[ -n "${GITHUB_WORKSPACE:-}" ]] && roots+=("$GITHUB_WORKSPACE")
[[ -n "${CHECKOUT:-}" ]] && roots+=("$CHECKOUT")
(( ${#roots[@]} > 0 )) || { echo "FAIL: neither GITHUB_WORKSPACE nor CHECKOUT names the checkout"; exit 1; }
fail=0
for r in "${roots[@]}"; do
    hits=$(strings -a "$bin" | grep -F -- "$r" | head -5)
    if [[ -n "$hits" ]]; then
        echo "FAIL: $bin carries the build machine's path $r:"
        sed 's/^/    /' <<<"$hits"
        fail=1
    fi
done
[[ $fail -eq 0 ]] && echo "OK: $bin carries no path of the machine that built it (${roots[*]})"
exit $fail
