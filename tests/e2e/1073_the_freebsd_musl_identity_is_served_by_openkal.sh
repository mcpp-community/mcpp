#!/usr/bin/env bash
# requires: llvm elf
# 1073 -- FreeBSD's musl identity is served by openkal (design 2026-11-10 E4, K8,
# K9; docs/21 'Build hosts and the targets they serve').
#
# `x86_64-freebsd-musl` is openkal-platform's hosted part (0.26.0): openkal-musl
# over openkal-freebsd in its standalone arrangement, linked as one static
# program with no C library of the system beneath. A request of `x86_64-freebsd`
# with no platform selected for it takes that identity, as `aarch64-macos`
# takes the musl identity on a host that is not a Mac; a project that selects a
# platform for `x86_64-freebsd` keeps the system's own ABI (e2e 1059).
#
# Criteria: `--target x86_64-freebsd-musl` builds a static FreeBSD ELF over
# openkal-musl and openkal-freebsd; `--target x86_64-freebsd` with no platform
# selected builds the same identity and says so.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; tail -30 "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
"$MCPP" new app > /dev/null 2>&1
cd app
"$MCPP" build --target x86_64-freebsd-musl > a.log 2>&1 || fail "A: x86_64-freebsd-musl did not build" a.log
grep -q 'openkal-musl@' a.log || fail "A: the C library is not openkal-musl" a.log
grep -q 'openkal-freebsd@' a.log || fail "A: the kernel layer is not openkal-freebsd" a.log
exe=$(find target/x86_64-freebsd-musl -path '*/bin/app' -type f | head -1)
[ -n "$exe" ] || fail "A: no program" a.log
info=$(file "$exe")
grep -q 'FreeBSD' <<<"$info" || fail "A: not a FreeBSD program: $info"
grep -q 'statically linked' <<<"$info" || fail "A: not static: $info"

"$MCPP" build --target x86_64-freebsd > b.log 2>&1 || fail "B: x86_64-freebsd with no platform did not build" b.log
grep -q 'x86_64-freebsd-musl' b.log || fail "B: the request does not say it took the musl identity" b.log
echo "PASS: 1073 the FreeBSD musl identity is served by openkal"
