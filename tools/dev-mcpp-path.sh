#!/usr/bin/env bash
# Print the mcpp binary produced by the build that just ran, and REFUSE to
# guess. target/ accumulates one directory per toolchain fingerprint, so
# `find … | head -1` answers a build you did not make — this repository has
# been bitten by that shape repeatedly. The marker makes "which build" a fact.
#
# ⚠️ RUN THE E2E SUITE AGAINST THIS PATH, NOT AGAINST A COPY. mcpp resolves
# MCPP_HOME from the binary's own location, so a binary copied elsewhere is a
# different binary as far as several tests are concerned —
# `30_dev_binary_home.sh` fails on a copy and passes in place, which reads as a
# regression and is not one. The reason to want a copy (the suite must not be
# rebuilt underneath itself) is real; the answer is to not rebuild while it
# runs, not to move the binary.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MARK="$ROOT/target/.dev-mcpp-marker"
case "${1:-}" in
  mark) mkdir -p "$ROOT/target"; : > "$MARK"; exit 0 ;;
esac
[[ -f "$MARK" ]] || { echo "no marker: run '$0 mark' before the build" >&2; exit 1; }
mapfile -t hits < <(find "$ROOT/target" -type f -name mcpp -perm -u+x -newer "$MARK")
if (( ${#hits[@]} != 1 )); then
    echo "expected exactly one freshly built mcpp, found ${#hits[@]}:" >&2
    printf '  %s\n' "${hits[@]}" >&2
    exit 1
fi
printf '%s\n' "${hits[0]}"
