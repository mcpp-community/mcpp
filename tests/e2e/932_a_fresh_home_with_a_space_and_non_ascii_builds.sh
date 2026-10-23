#!/usr/bin/env bash
# requires: gcc unix-shell fresh-sandbox
# A fresh home whose path holds a space builds the default project; one whose
# path also holds a non-ASCII character either builds or is refused naming
# gcc's module mapper (mcpp#832, R3).
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

# 1. A space: the chain must hold end to end.
home="$work/mc probe/home"
mkdir -p "$home"
export MCPP_HOME="$home"
out="$("$MCPP" self init 2>&1)" || fail "self init in a spaced home failed"
(cd "$work" && "$MCPP" new app >/dev/null 2>&1) || fail "mcpp new failed"
out="$(cd "$work/app" && "$MCPP" run 2>&1)" || fail "the default build in a spaced home failed"
grep -q "Hello" <<<"$out" || fail "the program did not run"
echo "  ok: a fresh home at '$home' builds and runs the default project"

# 2. A non-ASCII character as well. GCC's module mapper (libcody) cannot
#    parse its own encoding of a byte outside ASCII, so `import std` with gcc
#    cannot be built from a toolchain under such a path -- an upstream limit,
#    measured with gcc 16.1.0. What mcpp owes is a build that works, or a
#    refusal that names the cause.
home2="$work/mc probe/家"
mkdir -p "$home2"
export MCPP_HOME="$home2"
out="$("$MCPP" self init 2>&1)" || fail "self init in a non-ASCII home failed"
rm -rf "$work/app/target"
if out="$(cd "$work/app" && "$MCPP" run 2>&1)"; then
    grep -q "Hello" <<<"$out" || fail "the program did not run"
    echo "  ok: the non-ASCII home builds too"
else
    grep -q "gcc's module mapper cannot carry a path outside ASCII" <<<"$out" \
        || fail "the non-ASCII home failed without naming the cause"
    echo "  ok: the non-ASCII home is refused naming gcc's module mapper"
fi
echo "PASS: a fresh spaced home builds; a non-ASCII one builds or names the cause"
