#!/usr/bin/env bash
# requires: gcc
# 1003 -- a patched library is another library, and one program holds one
# variant of a package (design 2026-11-02 part 3 §14.1). Two programs of one
# workspace configuration may each use their own patch set of compat.zlib; a
# program that reaches two variants of it -- or a variant and the package
# without patches -- is refused, naming the variants.
#
# Criteria:
#   A. `one` (through member `a`, patch P1) and `two` (patch P2) build in one
#      configuration, and each sees its own patch.
#   B. `three`, reaching `a`'s variant and P2's, is refused naming both.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mark() {  # mark <file> <value>: a patch that defines MCPP_MARK as <value>
    printf '%s\n' '--- a/zlib-1.3.2/zlib.h' '+++ b/zlib-1.3.2/zlib.h' '@@ -31,4 +31,6 @@' \
        ' #ifndef ZLIB_H' ' #define ZLIB_H' ' ' "+#define MCPP_MARK $2" '+' ' #ifdef ZLIB_BUILD' > "$1"
}

cd "$TMP"
mkdir -p ws && cd ws
mkdir -p a/src one/src two/src three/src patches
mark patches/p1.patch 1
mark patches/p2.patch 2
printf '[workspace]\nmembers = ["a", "one", "two"]\n' > mcpp.toml
cat > a/mcpp.toml <<'EOF'
[package]
name    = "a"
version = "0.1.0"

[dependencies]
"compat.zlib" = { version = "1.3.2", patches = ["../patches/p1.patch"] }
EOF
cat > a/src/a.cppm <<'EOF'
module;
#include <zlib.h>
export module a;
export int a_mark() { return MCPP_MARK; }
EOF
printf '[package]\nname = "one"\nversion = "0.1.0"\n\n[dependencies]\na = { path = "../a" }\n' > one/mcpp.toml
printf 'import std;\nimport a;\nint main() { std::println("one {}", a_mark()); }\n' > one/src/main.cpp
cat > two/mcpp.toml <<'EOF'
[package]
name    = "two"
version = "0.1.0"

[dependencies]
"compat.zlib" = { version = "1.3.2", patches = ["../patches/p2.patch"] }
EOF
printf '#include <zlib.h>\n#include <cstdio>\nint main() { std::printf("two %%d %%s\\n", MCPP_MARK, zlibVersion()); }\n' > two/src/main.cpp

# A
"$MCPP" build --workspace > a.log 2>&1 || fail "two programs with two variants did not build" a.log
"$MCPP" run -p one > one.log 2>&1 || fail "one did not run" one.log
"$MCPP" run -p two > two.log 2>&1 || fail "two did not run" two.log
grep -q '^one 1' one.log || fail "one did not see its patch" one.log
grep -q '^two 2 1.3.2' two.log || fail "two did not see its patch" two.log
[ "$(ls -d "$MCPP_HOME"/cache/src/compat.zlib/1.3.2+p* | wc -l)" = 2 ] || fail "the two variants do not have a tree each" <(ls "$MCPP_HOME/cache/src/compat.zlib")

# B
printf '[workspace]\nmembers = ["a", "one", "two", "three"]\n' > mcpp.toml
cat > three/mcpp.toml <<'EOF'
[package]
name    = "three"
version = "0.1.0"

[dependencies]
a = { path = "../a" }
"compat.zlib" = { version = "1.3.2", patches = ["../patches/p2.patch"] }
EOF
printf 'import a;\nint main() { return a_mark() == 0; }\n' > three/src/main.cpp
if "$MCPP" build -p three > b.log 2>&1; then fail "a program with two variants was accepted" b.log; fi
grep -q "reaches two variants of 'compat.zlib'" b.log || fail "the refusal does not name the package" b.log
[ "$(grep -c 'compat.zlib+p' b.log)" -ge 2 ] || fail "the refusal does not name both variants" b.log

echo "OK"
