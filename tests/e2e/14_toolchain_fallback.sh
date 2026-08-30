#!/usr/bin/env bash
# requires:
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

# A project-level [toolchain] = "system" is REFUSED, and refused on its own
# terms rather than falling through to "no toolchain configured".
#
# ⚠️ THIS USED TO ASSERT THE OPPOSITE — that `system` "still works as escape
# hatch" — and the assertion was only ever `grep -q 'no toolchain configured'`
# being absent. That predicate stays satisfied by ANY other error, so when the
# escape hatch became a refusal the test went on passing while its stated
# intent had inverted. A negative-only assertion cannot tell "it worked" from
# "it failed differently"; both halves are checked now.
#
# The policy: mcpp builds only with toolchains it manages. A compiler taken
# from PATH cannot be identified or reproduced, so `import std` availability
# and "the same build on another machine" stop being things mcpp can promise.
# `msvc@system` is the single exception and is not this spelling. Host
# LIBRARIES are a different axis and remain the project's own choice.
cat >> mcpp.toml <<'EOF'

[toolchain]
default = "system"
EOF
out=$("$MCPP" build 2>&1) || true
echo "$out" | grep -q 'no toolchain configured' && {
    echo "FAIL: 'system' fell through to the unconfigured path: $out"; exit 1; }
echo "$out" | grep -q 'toolchains it manages' || {
    echo "FAIL: 'system' was not refused on its own terms: $out"; exit 1; }
echo "$out" | grep -q 'msvc@system' || {
    echo "FAIL: the refusal does not name the one supported exception: $out"; exit 1; }

echo "OK"
