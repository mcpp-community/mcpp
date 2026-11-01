#!/usr/bin/env bash
# requires: gcc
# 1001 -- a dependency edge carries patches (design 2026-11-02 part 3 §14.1). A
# patched library is another library: the verified source is copied to the
# variant's own tree under the mcpp home and patched there, in order, exactly;
# the lock names the variant and each patch with its SHA-256; and a patch that
# does not apply is refused naming the patch and its hunk.
#
# Criteria:
#   A. The program sees the patched header; the package's own installed source
#      is untouched, and the variant's tree is beside the other variants.
#   B. mcpp.lock records the variant, and each patch by its path and SHA-256;
#      `mcpp sbom` states the pedigree.
#   C. A changed patch is another variant: the build uses the new tree.
#   D. A patch that does not apply is refused with the patch's name and the
#      hunk, and leaves no variant behind.
#   E. `patches` on a `path` dependency is refused.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
mkdir -p app/src app/patches && cd app
printf '%s\n' '--- a/zlib-1.3.2/zlib.h' '+++ b/zlib-1.3.2/zlib.h' '@@ -31,4 +31,6 @@' \
    ' #ifndef ZLIB_H' ' #define ZLIB_H' ' ' '+#define MCPP_PATCHED_ZLIB 1' '+' ' #ifdef ZLIB_BUILD' \
    > patches/0001-mark.patch
cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
"compat.zlib" = { version = "1.3.2", patches = ["patches/0001-mark.patch"] }
EOF
cat > src/main.cpp <<'EOF'
#include <zlib.h>
#include <cstdio>
int main() {
#ifdef MCPP_PATCHED_ZLIB
    std::printf("patched %d %s\n", MCPP_PATCHED_ZLIB, zlibVersion());
    return 0;
#else
    std::printf("unpatched\n");
    return 1;
#endif
}
EOF

# A
"$MCPP" run > a.log 2>&1 || fail "the patched build did not run" a.log
grep -q '^patched 1 1.3.2' a.log || fail "the program did not see the patched header" a.log
variant=$(ls -d "$MCPP_HOME"/cache/src/compat.zlib/1.3.2+p* 2>/dev/null | head -1)
[ -n "$variant" ] && [ -f "$variant/.mcpp-variant" ] || fail "no variant tree under the mcpp home" <(find "$MCPP_HOME/cache" -maxdepth 3)
grep -q 'MCPP_PATCHED_ZLIB' "$variant/zlib-1.3.2/zlib.h" || fail "the variant's header is not patched" "$variant/zlib-1.3.2/zlib.h"
installed=$(ls -d "$MCPP_HOME"/registry/data/xpkgs/compat-x-zlib/1.3.2 2>/dev/null | head -1)
if [ -n "$installed" ]; then
    ! grep -q 'MCPP_PATCHED_ZLIB' "$installed"/zlib-1.3.2/zlib.h || fail "the installed source was patched in place"
fi

# B
grep -q '^variant = ' mcpp.lock || fail "the lock names no variant" mcpp.lock
grep -q 'patches/0001-mark.patch#sha256:[0-9a-f]\{64\}' mcpp.lock || fail "the lock does not name the patch and its digest" mcpp.lock
"$MCPP" emit sbom > sbom.json 2>sbom.err || fail "sbom failed" sbom.err
grep -q '"pedigree"' sbom.json && grep -q '0001-mark.patch' sbom.json || fail "the bill of materials states no pedigree" sbom.json

# C
sed -i 's/MCPP_PATCHED_ZLIB 1/MCPP_PATCHED_ZLIB 2/' patches/0001-mark.patch
"$MCPP" run > c.log 2>&1 || fail "the build with a changed patch failed" c.log
grep -q '^patched 2 ' c.log || fail "a changed patch did not make another variant" c.log
[ "$(ls -d "$MCPP_HOME"/cache/src/compat.zlib/1.3.2+p* | wc -l)" -ge 2 ] || fail "the changed patch did not get its own tree" <(ls "$MCPP_HOME/cache/src/compat.zlib")

# D
cat > patches/0001-mark.patch <<'EOF'
--- a/zlib-1.3.2/zlib.h
+++ b/zlib-1.3.2/zlib.h
@@ -31,1 +31,1 @@
-#ifndef NOT_THE_LINE
+#ifndef ZLIB_H
EOF
before=$(ls -d "$MCPP_HOME"/cache/src/compat.zlib/* | wc -l)
if "$MCPP" build > d.log 2>&1; then fail "a patch that does not apply was accepted" d.log; fi
grep -q "0001-mark.patch" d.log && grep -q "hunk 1" d.log || fail "the refusal does not name the patch and the hunk" d.log
[ "$(ls -d "$MCPP_HOME"/cache/src/compat.zlib/* | wc -l)" = "$before" ] || fail "a failed patching left a tree" <(ls "$MCPP_HOME/cache/src/compat.zlib")

# E
mkdir -p ../lib/src
printf '[package]\nname = "lib"\nversion = "0.1.0"\n' > ../lib/mcpp.toml
printf 'int lib_one() { return 1; }\n' > ../lib/src/lib.cpp
cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
lib = { path = "../lib", patches = ["patches/0001-mark.patch"] }
EOF
if "$MCPP" build > e.log 2>&1; then fail "patches on a path dependency were accepted" e.log; fi
grep -q "is a path and carries patches" e.log || fail "the refusal does not say why" e.log

echo "OK"
