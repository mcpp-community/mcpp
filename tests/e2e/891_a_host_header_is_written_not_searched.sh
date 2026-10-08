#!/usr/bin/env bash
# requires: llvm elf
# 891 -- the host's /usr/include is not searched; a project that needs it
# writes it down (2026.10.8.1, LLVM 23.1.3 Part 3, D2/D3).
#
# A clang build on a managed glibc carries `-nostdlibinc`: the payload
# supplies the whole system header surface, and the driver's fallback to the
# host's /usr/include is closed. GCC already never searched it (its default
# search ends at the xlings subos). A header that only the host has is
# therefore not found, and the answer is a line in the manifest rather than a
# switch: the directory is named where it is used.
#
#   A  a host-only header is not found, and the failure does NOT carry the
#      note for a graph-supplied C library (the C library here is the
#      payload's), on the full build and on the fast path alike;
#   B  `-idirafter /usr/include` in `cxxflags` makes it visible again.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

header=""
for h in zlib.h X11/Xlib.h curl/curl.h openssl/ssl.h; do
    [ -f "/usr/include/$h" ] && { header=$h; break; }
done
if [ -z "$header" ]; then
    echo "SKIP: 891 needs a header that only the host's /usr/include has"
    exit 0
fi

mkdir -p "$TMP/app/src"
cd "$TMP/app"
write_manifest() {
    printf '[package]\nname = "hosthdr"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "llvm@%s"\n%s' \
        "$LLVM_VERSION" "${1:-}" > mcpp.toml
}
write_manifest
printf '#include <%s>\n#include <cstdio>\nint main() { std::puts("host header seen"); }\n' \
    "$header" > src/main.cpp

# A: not found, and not explained as a graph C library.
if "$MCPP" build > a.log 2>&1; then fail "A: <$header> was found in the host's /usr/include" a.log; fi
grep -q "file not found" a.log || fail "A: the failure is not the missing header" a.log
grep -q "comes from the dependency graph" a.log \
    && fail "A: a payload C library was described as the graph's" a.log
# The same failure through the fast path (build.ninja already written).
if "$MCPP" build > a2.log 2>&1; then fail "A: the second build succeeded" a2.log; fi
grep -q "comes from the dependency graph" a2.log \
    && fail "A: the fast path described a payload C library as the graph's" a2.log

# B: the directory, written down, is searched after the payload's headers.
write_manifest $'\n[build]\ncxxflags = ["-idirafter", "/usr/include"]\n'
"$MCPP" run > b.log 2>&1 || fail "B: an explicit -idirafter /usr/include did not build" b.log
grep -q "host header seen" b.log || fail "B: the program did not run" b.log

echo "PASS: 891 a host header is written down, not searched"
