#!/usr/bin/env bash
# requires: windows
# mcpp#365 — native Windows: [resources] compiles to a .res through llvm-rc or
# rc.exe and is linked in by lld-link/link.exe. Shared assertions live in
# _windows_resources_body.sh; 198 runs the same ones through the GNU/windres
# fork so the two dialects cannot drift.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

BUILD_ARGS=""
EXE_SUFFIX=".exe"
source "$(dirname "$0")/_windows_resources_body.sh"
