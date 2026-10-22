#!/usr/bin/env bash
# requires: gcc unix-shell fresh-sandbox
# A fresh home whose path holds a space and a non-ASCII character builds the
# default project (mcpp#832, R3).
#
# The failure #832 reported was the gcc payload's install hook: the index's
# recipe interpolated the home's path into a shell command unquoted, so the
# loader path it passed was split at the space. The recipe now quotes every
# interpolated path (xim-pkgindex `libs/shell.lua`, its lint), and this test
# holds the whole chain -- home, provider, payload hooks, link -- to a path
# that is not a single plain word.
set -e

MCPP="${MCPP:-mcpp}"
source "$(dirname "$0")/_toolchain_env.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export NO_COLOR=1
fail() { echo "FAIL: $*"; echo "----"; echo "${out:-}"; exit 1; }

home="$work/mc probe/家"
mkdir -p "$home"
export MCPP_HOME="$home"
out="$("$MCPP" self init 2>&1)" || fail "self init in a spaced, non-ASCII home failed"
"$MCPP" new app >/dev/null 2>&1 || fail "mcpp new failed"
cd app
out="$("$MCPP" run 2>&1)" || fail "the default build in a spaced, non-ASCII home failed"
grep -q "Hello" <<<"$out" || fail "the program did not run"
echo "PASS: a fresh home at '$home' builds and runs the default project"
