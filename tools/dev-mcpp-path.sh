#!/usr/bin/env bash
# Print the mcpp binary produced by the build that just ran, and REFUSE to
# guess. target/ accumulates one directory per toolchain fingerprint, so
# `find … | head -1` answers a build you did not make — this repository has
# been bitten by that shape repeatedly. The marker makes "which build" a fact.
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
