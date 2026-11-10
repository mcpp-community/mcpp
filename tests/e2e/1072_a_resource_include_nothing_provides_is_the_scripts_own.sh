#!/usr/bin/env bash
# requires: mingw-cross
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1072 -- a resource script's quoted include that no directory holds and no build action writes is
# the script's own matter, not an input of the build graph.
#
# The scan of a script records each quoted include it finds in the script's directories, else
# against the script's directory, where a file a build action produces is ordered before the
# compile. A name nothing produces became a graph input no rule makes, and the build failed in
# ninja without the script's name (`ninja: error: '<project>/absent.h', needed by 'res/app.o',
# missing and no known rule to make it`), even when the script includes it only
# `#if __has_include`. Such a name is now left to the script's preprocessing and to the resource
# compiler, which names the script and its line; a name a build action writes stays an input.
#
# Criteria: a script that includes an absent header under `__has_include` builds; one that
# includes it unconditionally fails with the resource compiler's message naming the script.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/src"
cat > "$t/mcpp.toml" <<'TOML'
[package]
name    = "rcinc"
version = "0.1.0"

[resources]
files = ["app.rc"]

[targets.rcinc]
kind = "bin"
main = "src/main.c"
TOML
echo 'int main(void) { return 0; }' > "$t/src/main.c"
cat > "$t/app.rc" <<'RC'
#if __has_include("absent.h")
#include "absent.h"
#endif
1 VERSIONINFO
FILEVERSION 1,0,0,0
BEGIN
END
RC
cd "$t"
out=$("$MCPP" build --target x86_64-windows-gnu 2>&1) \
    || fail "a script that includes an absent header under __has_include did not build" "$out"
find target/x86_64-windows-gnu -path '*/bin/rcinc.exe' -type f | grep -q . || fail "no program"
# Unconditional: the resource compiler's error, naming the script.
sed -i '1d;3d' app.rc
if out=$("$MCPP" build --target x86_64-windows-gnu 2>&1); then fail "an absent header included unconditionally built" "$out"; fi
grep -q 'ninja: error' <<<"$out" && fail "the build failed in ninja, not in the resource compiler" "$out"
grep -q 'app.rc' <<<"$out" || fail "the failure does not name the script" "$out"
echo "PASS: 1072 a resource include nothing provides is the script's own"
