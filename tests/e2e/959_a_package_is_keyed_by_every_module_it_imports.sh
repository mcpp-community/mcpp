#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 959 -- a cached package's key covers every package whose module it imports,
# a declared edge or not.
#
# A BMI is bound to the BMIs it read. The key of a cached package folded in the
# keys of the dependencies its manifest states, and a package can import a
# module its manifest does not name: the libraries of a build program import
# the engine interface (`import mcpp;`), which the engine adds to the build
# program's graph. Served under another interface, such an entry failed with
# "import 'mcpp' has CRC mismatch" (mcpp.plugins, 2026.10.28.1 over a cache
# 2026.10.27.1 builds had filled).
#
# Here the engine's part is played by an index package the importer does not
# declare: `ex.lib` imports `ex.api`, and the project depends on both.
#
# Criteria:
#   A. With ex.api 0.1.0 the project builds, and ex.lib is cached.
#   B. With ex.api 0.2.0 (another interface) and the same ex.lib, the project
#      builds: ex.lib is compiled again against the interface it imports, and
#      the entry compiled against 0.1.0 is not served.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

descriptor() {  # <name> <versions...>: a package served from a seeded payload
    local name=$1; shift
    local rows=""
    for v in "$@"; do
        rows+="            [\"$v\"] = { url = \"https://example.invalid/$name-$v.tar.gz\", sha256 = \"$(printf '0%.0s' {1..64})\" },"$'\n'
    done
    cat > "$TMP/index/pkgs/e/ex.$name.lua" <<EOF
package = {
    spec = "1",
    namespace = "ex",
    name = "ex.$name",
    description = "959",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = {
$rows        },
        macosx  = {
$rows        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/$name.cppm" },
        targets = { ["$name"] = { kind = "lib" } },
        deps = {},
    },
}
EOF
}
mkdir -p "$TMP/index/pkgs/e"
descriptor api 0.1.0 0.2.0
descriptor lib 0.1.0

mkdir -p "$TMP/app/src"
cd "$TMP/app"
store=".mcpp/.xlings/data/xpkgs"
mkdir -p "$store/ex.api/0.1.0/src" "$store/ex.api/0.2.0/src" "$store/ex.lib/0.1.0/src"
cat > "$store/ex.api/0.1.0/src/api.cppm" <<'EOF'
export module ex.api;
export namespace api { inline int value() { return 1; } }
EOF
cat > "$store/ex.api/0.2.0/src/api.cppm" <<'EOF'
export module ex.api;
export namespace api { struct Box { int v; }; inline int value() { return Box{2}.v; } }
EOF
# ex.lib imports ex.api and states no dependency on it.
cat > "$store/ex.lib/0.1.0/src/lib.cppm" <<'EOF'
export module ex.lib;
import ex.api;
export namespace lib { inline int twice() { return 2 * api::value(); } }
EOF
cat > src/main.cpp <<'EOF'
import ex.lib;
int main() { return lib::twice() == EXPECT ? 0 : 1; }
EOF
manifest() {  # <api version> <expected value>
    cat > mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[build]
cxxflags = ["-DEXPECT=$2"]

[dependencies]
"ex.api" = "=$1"
"ex.lib" = "=0.1.0"

[indices]
ex = { path = "$TMP/index" }
EOF
}

# A
manifest 0.1.0 2
"$MCPP" run > a.log 2>&1 || fail "A: the project over ex.api 0.1.0" a.log
ls "$MCPP_HOME"/build-cache/v1/pkg/*/ex.lib@0.1.0/ > /dev/null 2>&1 || fail "A: ex.lib was not cached" a.log
echo "ok: ex.lib is cached over ex.api 0.1.0"

# B
manifest 0.2.0 4
"$MCPP" run > b.log 2>&1 || fail "B: the project over ex.api 0.2.0 with the same ex.lib" b.log
grep -q "CRC mismatch" b.log && fail "B: ex.lib compiled against ex.api 0.1.0 was served" b.log
[ "$(ls -d "$MCPP_HOME"/build-cache/v1/pkg/*/ex.lib@0.1.0/*/ | wc -l)" -eq 2 ] \
    || fail "B: ex.lib has not one entry per interface it imported" b.log
echo "ok: ex.lib is compiled again against the interface it imports"

echo "PASS: 959 a package is keyed by every module it imports"
