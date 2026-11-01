#!/usr/bin/env bash
# requires: gcc
# 1002 -- `embed = true` compiles a dependency into its consumer (design
# 2026-11-02 part 3 §14.1): the dependency's sources, less `exclude`, are the
# consumer's, compiled with the consumer's flags and the dependency's include
# directories and macros; the dependency builds no library of its own, and a
# program that reaches it holds its embedder or is refused.
#
# Criteria:
#   A. The program links and runs with zlib embedded in its library package;
#      no library of compat.zlib is built, and an excluded file is not compiled.
#   B. The dependency's own compile flags reach its files (its `-include` of a
#      generated header) and not the consumer's own.
#   C. `exclude` without `embed` is refused at the manifest.
#   D. A program that reaches the embedded package without its embedder is
#      refused, naming both.
#   E. An embedder with a build program builds without a warning about the
#      flags `embed' gives the dependency's files: the build program's own plan
#      compiles none of them, and the entry is the engine's, not a typo.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
mkdir -p zip/src app/src
cat > zip/mcpp.toml <<'EOF'
[package]
name    = "zip"
version = "0.1.0"

[dependencies]
"compat.zlib" = { version = "1.3.2", embed = true, exclude = ["*/uncompr.c"] }
EOF
cat > zip/src/zip.cppm <<'EOF'
module;
#include <zlib.h>
export module zip;
export int zip_bound(int n) { return static_cast<int>(compressBound(static_cast<uLong>(n))); }
export const char* zip_version() { return zlibVersion(); }
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
zip = { path = "../zip" }
EOF
cat > app/src/main.cpp <<'EOF'
import std;
import zip;
int main() { std::println("{} {}", zip_version(), zip_bound(100) > 100); }
EOF

cat > zip/build.mcpp <<'EOF'
import std;
int main() { return 0; }
EOF

# A
cd app
"$MCPP" run > a.log 2>&1 || fail "the program with an embedded zlib did not run" a.log

# E
! grep -q 'matched no source file' a.log || fail "the flags embed gives were reported as a glob that matched nothing" a.log
grep -q '^1.3.2 true' a.log || fail "the program did not call the embedded zlib" a.log
dir=$(ls -d target/*/*/ | grep -E '/[0-9a-f]{16}/$' | head -1)
db=$(find "$dir" -name compile_commands.json | head -1)
[ -n "$db" ] || db=compile_commands.json
grep -q 'deflate.c' "$db" || fail "zlib's sources were not compiled" "$db"
! grep -q 'uncompr.c' "$db" || fail "an excluded file was compiled" "$db"
! find "$dir" -name '*zlib*.a' -o -name '*zlib*.lib' | grep -q . || fail "a library of the embedded package was built" <(find "$dir" -name '*.a')

# B
grep -q 'mcpp_zlib_config.h' "$db" || fail "the dependency's own flags did not reach its files" "$db"
python3 - "$db" <<'PY' || fail "the dependency's flags reached the consumer's own unit" "$db"
import json, sys
for e in json.load(open(sys.argv[1])):
    if e["file"].endswith("zip.cppm") and "mcpp_zlib_config.h" in " ".join(e.get("arguments", [])) + e.get("command", ""):
        sys.exit(1)
PY
cd ..

# C
mkdir -p bad/src && cd bad
printf '[package]\nname = "bad"\nversion = "0.1.0"\n\n[dependencies]\n"compat.zlib" = { version = "1.3.2", exclude = ["*/gzwrite.c"] }\n' > mcpp.toml
printf 'int main() {}\n' > src/main.cpp
if "$MCPP" build > c.log 2>&1; then fail "exclude without embed was accepted" c.log; fi
grep -q "exclude is allowed only with embed" c.log || fail "the refusal does not say why" c.log
cd ..

# D -- one configuration of a workspace: `app` holds the embedder, `plain`
# reaches compat.zlib directly. The embedded package compiles nothing of its
# own there, so `plain` would link its declarations and no definitions.
mkdir -p ws/plain/src && cd ws
cp -r ../app ../zip .
rm -rf app/target
printf '[workspace]\nmembers = ["app", "plain", "zip"]\n' > mcpp.toml
printf '[package]\nname = "plain"\nversion = "0.1.0"\n\n[dependencies]\n"compat.zlib" = "1.3.2"\n' > plain/mcpp.toml
printf '#include <zlib.h>\nint main() { return zlibVersion()[0] == 0; }\n' > plain/src/main.cpp
if "$MCPP" build --workspace > d.log 2>&1; then fail "a program without the embedder was accepted" d.log; fi
grep -q "which 'zip' embeds" d.log || fail "the refusal does not name the embedder" d.log
cd ..

echo "OK"
