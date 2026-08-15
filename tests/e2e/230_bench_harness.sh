#!/usr/bin/env bash
# requires: python3
# Delegates to the bench suite's own test.
#
# The suite lives in `bench/` and is meant to be extractable into its own
# project one day, so its tests live there too — mixing them into mcpp's e2e
# directory would make that separation a rename away from breaking.
#
# This delegator stays because deleting it would silently drop the harness from
# every mcpp PR that does not touch bench/: `bench.yml` is PATH-SCOPED to
# `bench/**`, so a change elsewhere that breaks the suite runs nothing.
set -e
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# The e2e runner exports MCPP as the binary under test and that always wins.
# Filling it in when unset is what makes this script runnable BY HAND, which the
# harness cannot do for itself: it must not fall back to PATH (that resolves to
# the xlings shim, which re-picks a version per working directory), but this
# delegator lives in mcpp's own tree and can simply point at what was built.
if [ -z "${MCPP:-}" ]; then
  MCPP="$(bash "$REPO/.github/tools/newest_artifact.sh" "$REPO" mcpp 2>/dev/null || true)"
  [ -n "$MCPP" ] || { echo "SKIP: no mcpp binary built yet — run \`mcpp build\` first"; exit 0; }
  export MCPP
fi
exec bash "$REPO/bench/tests/harness.sh"
