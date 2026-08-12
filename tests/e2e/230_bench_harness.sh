#!/usr/bin/env bash
# requires: python3
# Delegates to the bench suite's own test.
#
# The suite lives in `bench/` and is meant to be extractable into its own
# project one day, so its tests live there too — mixing them into mcpp's e2e
# directory would make that separation a rename away from breaking.
#
# This delegator stays because deleting it would silently drop the harness from
# every mcpp PR: `bench.yml` is workflow_dispatch-only, so nothing else runs it.
set -e
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec bash "$REPO/bench/tests/harness.sh"
