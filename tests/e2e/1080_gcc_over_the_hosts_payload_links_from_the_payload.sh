#!/usr/bin/env bash
# requires: gcc elf
# 1080 -- gcc over the host's C library payload names the payload on its link:
# the crt search prefix, the library directory and the loader (design
# 2026-11-12 C1; SPEC-006).
#
# A gcc whose recorded sysroot does not exist on this machine is linked
# "payload first": the link names the glibc payload's directory for the crt
# objects (`-B`), for the libraries (`-L`, with its run-time path) and for the
# loader. A rewrite of that branch of the flag renderer once dropped the whole
# group, and the crt objects then resolved from the host's /lib -- on a machine
# that had them; the hermetic link check refused the build. No test of the tree
# built with such a gcc, so the first report was a failed build elsewhere.
#
# gcc 15.1.0 records the sysroot of the machine that built it, which exists on
# no other; the engine's own gcc line (16.x) records the subos root and is
# linked through `--sysroot`. Criteria: a project built with gcc@15.1.0 links,
# its link line names the payload three times as above, and the program runs.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -25; exit 1; }

cd "$t"
"$MCPP" new app > /dev/null 2>&1
cd app
out=$("$MCPP" build --toolchain gcc@15.1.0 2>&1) || fail "the project did not build with gcc@15.1.0" "$out"
ninja=$(find target -name build.ninja | head -1)
[ -n "$ninja" ] || fail "no build.ninja" "$out"
line=$(grep -m1 -E '^ *ldflags *=' "$ninja") || fail "no link flags in build.ninja" "$(head -40 "$ninja")"
grep -q -- '-B[^ ]*xim-x-glibc' <<<"$line" || fail "the link names no crt search prefix in the payload" "$line"
grep -q -- '-L[^ ]*xim-x-glibc' <<<"$line" || fail "the link names no library directory in the payload" "$line"
grep -q -- '--dynamic-linker=[^ ]*xim-x-glibc' <<<"$line" || fail "the link names no loader in the payload" "$line"
if grep -q -- '--sysroot=' <<<"$line"; then
    fail "this gcc was linked through a sysroot: the script holds the payload-first form" "$line"
fi
run=$("$MCPP" run --toolchain gcc@15.1.0 2>&1) || fail "the program did not run" "$run"
grep -q "Hello" <<<"$run" || fail "the program's output did not arrive" "$run"
echo "PASS: 1080 gcc over the host's payload links from the payload"
