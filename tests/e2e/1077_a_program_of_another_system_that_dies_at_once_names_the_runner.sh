#!/usr/bin/env bash
# requires: llvm
# 1077 -- a program of another system's kernel interface that dies at once
# names the runner that would run it (2026.11.11.1, G6/G7).
#
# `mcpp run --target x86_64-freebsd-musl` on a Linux host spawns a static
# FreeBSD ELF of the host's own architecture: the kernel maps it without
# asking which system it names, the program states its first kernel call in
# FreeBSD's vocabulary, and the death is by signal -- 139, with nothing to
# read. `mcpp run` now runs such an artifact as a child, so the death is
# observed and named with the [target.<triple>] runner declaration; the
# status itself is returned unchanged, exactly what a direct launch would
# have reported.
#
# Criteria: the exit status stays 139; the error text states the death, the
# other system's kernel interface, and the runner declaration for the row.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; tail -30 "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
"$MCPP" new app > /dev/null 2>&1
cd app
"$MCPP" build --target x86_64-freebsd-musl > b.log 2>&1 || fail "the static FreeBSD program did not build" b.log
exe=$(find target/x86_64-freebsd-musl -path '*/bin/app' -type f | head -1)
[ -n "$exe" ] || fail "no program" b.log

set +e
"$MCPP" run --target x86_64-freebsd-musl > run.log 2>&1
rc=$?
set -e
[ "$rc" -eq 139 ] || fail "run exited $rc, expected the program's own death status 139" run.log
grep -q "was started and died at once" run.log \
    || fail "the run does not state the death at once" run.log
grep -q "another system's kernel interface" run.log \
    || fail "the run does not state the kernel interface fact" run.log
grep -q "\[target.x86_64-freebsd-musl\]" run.log \
    || fail "the runner declaration names the wrong target" run.log
grep -q "runner = \[" run.log \
    || fail "no runner declaration is printed" run.log
grep -q -- "--no-runner" run.log \
    || fail "the direct-execution escape hatch is not named" run.log
echo "PASS: 1077 a program of another system that dies at once names the runner"
