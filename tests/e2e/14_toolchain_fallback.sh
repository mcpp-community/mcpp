#!/usr/bin/env bash
# 14_toolchain_fallback.sh — M5.5: when no toolchain is configured at all
# (no project [toolchain], no global default), `mcpp build` hard-errors with
# a helpful message instead of falling back to system PATH.
#
# As of the first-run UX, mcpp DEFAULT auto-installs musl-gcc
# on the first build (covered by tests/e2e/29). Here we opt out via
# MCPP_NO_AUTO_INSTALL to assert the hard-error path still exists for
# CI / offline scenarios.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"   # fresh = no global default
export MCPP_NO_AUTO_INSTALL=1       # opt out of first-run auto-install

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp

rc=0
out=$("$MCPP" build 2>&1) || rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL: build should hard-error without toolchain"; exit 1; }

# Helpful error message.
echo "$out" | grep -q 'no toolchain configured' || {
    echo "FAIL: error message not helpful: $out"; exit 1; }
echo "$out" | grep -q 'mcpp toolchain install' || {
    echo "FAIL: error doesn't suggest install: $out"; exit 1; }

# Adding a project-level [toolchain] = "system" still works as escape hatch.
cat >> mcpp.toml <<'EOF'

[toolchain]
default = "system"
EOF
out=$("$MCPP" build 2>&1) || true
# 'system' opt-in path uses $CXX or PATH g++ — host-dependent; we don't
# verify success, only that the hard-error path doesn't fire.
echo "$out" | grep -q 'no toolchain configured' && {
    echo "FAIL: 'system' opt-in still hard-errored: $out"; exit 1; }

echo "OK"
