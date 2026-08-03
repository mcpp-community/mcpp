#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 184_build_cache_object_layout.sh — mcpp#344.
#
# A dependency's objects live in the global cache under a key that deliberately
# excludes the consumer. So the LAYOUT of those objects inside the entry must
# also exclude the consumer — otherwise two projects that share a key disagree
# about where the files are, and whichever runs SECOND asks the entry for a path
# the first never wrote.
#
# That is what happened. Basename disambiguation (#233) was decided by a census
# over every unit in the build directory, so `lib-a`'s `compress.c` compiled to
#     obj/compress.o                        in a project that pulls lib-a alone
#     obj/lib_a/src/compress.o              in one that also pulls lib-b
#                                           (lib-b ships its own compress.c)
# with the SAME cache key. The second project's build died in ninja's GRAPH
# phase — before a single command ran — with
#     ninja: error: '<cache>/…/obj/compress.o', needed by 'obj/compress.o',
#            missing and no known rule to make it
# one line after the CLI printed "Cached lib-a (N units)".
#
# The test runs both orderings, because the defect is symmetric: whichever
# project is second is the one that breaks.
#
# TWO assertions per direction, and the second one is not optional:
#   1. the build succeeds
#   2. the reused dependency has ZERO compile edges
# Without (2) this test would pass on a build that merely degraded every hit to
# a miss — which is a real regression (the cache silently stops paying for
# itself) that "it built fine" cannot see.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/local-index"
mkdir -p "$INDEX_DIR/pkgs/l"

# ── two library packages that COLLIDE on a source basename ───────────────────
# Neither package can avoid this: upstream zlib and upstream bzip2 both ship a
# file called compress.c, and a descriptor has no field that controls object
# paths. The collision has to be handled by mcpp or not at all.
make_descriptor() {   # $1 = package name
    cat > "$INDEX_DIR/pkgs/l/$1.lua" <<EOF
package = {
    spec = "1",
    name = "$1",
    description = "collides on compress.c",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/$1-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        sources = { "src/**/*.c" },
        targets = { ["$1"] = { kind = "lib" } },
        deps = {},
    },
}
EOF
}
make_descriptor lib-a
make_descriptor lib-b

# `make_project <dir> <name> <with-lib-b>`
make_project() {
    local dir="$1" name="$2" withb="$3"
    local proj="$TMP/$dir"
    mkdir -p "$proj/src"

    local payload="$proj/.mcpp/.xlings/data/xpkgs"
    mkdir -p "$payload/local-dev.lib-a/1.0.0/src"
    cat > "$payload/local-dev.lib-a/1.0.0/src/compress.c" <<'EOF'
int a_value(void) { return 40; }
EOF
    local deps='"local-dev.lib-a" = "1.0.0"'
    local extern='extern "C" int a_value(void);'
    local expr='a_value() + 2'
    if [[ "$withb" == "yes" ]]; then
        mkdir -p "$payload/local-dev.lib-b/1.0.0/src"
        cat > "$payload/local-dev.lib-b/1.0.0/src/compress.c" <<'EOF'
int b_value(void) { return 1; }
EOF
        deps="$deps"$'\n'
        deps="$deps"'"local-dev.lib-b" = "1.0.0"'
        extern="$extern"$'\n''extern "C" int b_value(void);'
        expr='a_value() + b_value() + 1'
    fi

    cat > "$proj/src/main.cpp" <<EOF
import std;
$extern
int main() { std::println("{}", $expr); return 0; }
EOF
    cat > "$proj/mcpp.toml" <<EOF
[package]
name = "$name"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR" }

[dependencies]
$deps

[targets.$name]
kind = "bin"
main = "src/main.cpp"
EOF
}

find_ninja()  { find "$1/target" -name build.ninja | head -1; }
a_compile_edges() { grep -cE ': (cxx_module|cxx_object|c_object|cxx_scan) .*lib-a' "$1" || true; }
a_stage_edges()   { grep -cE '^build .* : stage_file .*lib-a' "$1" || true; }

# Both projects must print 42, so a wrongly staged object shows up as a wrong
# answer rather than as a build that merely linked.
check_project() {   # $1 = dir, $2 = name, $3 = "reuses" | "cold"
    local dir="$1" name="$2" mode="$3"
    cd "$TMP/$dir"
    rm -rf target
    "$MCPP" build > build.log 2>&1 || {
        echo "FAIL: $dir failed to build ($mode)"
        cat build.log
        exit 1
    }
    local nj; nj="$(find_ninja "$TMP/$dir")"
    [[ -n "$nj" ]] || { echo "FAIL: $dir has no build.ninja"; exit 1; }

    if [[ "$mode" == "reuses" ]]; then
        local edges; edges="$(a_compile_edges "$nj")"
        if [[ "$edges" != "0" ]]; then
            echo "FAIL: $dir recompiled lib-a ($edges compile edges) instead of"
            echo "      reusing the entry the other project populated."
            echo "      A hit that silently became a miss is still a regression:"
            echo "      the cache stops paying for itself with no signal."
            grep -nE ': (cxx_module|cxx_object|c_object|cxx_scan) .*lib-a' "$nj" | head
            exit 1
        fi
        local staged; staged="$(a_stage_edges "$nj")"
        [[ "$staged" -gt 0 ]] || {
            echo "FAIL: $dir has neither compile nor stage edges for lib-a"
            grep -n 'lib-a' "$nj" | head
            exit 1
        }
    fi

    ./target/*/*/bin/"$name" > run.log 2>&1 || { cat run.log; exit 1; }
    grep -q '^42$' run.log || {
        echo "FAIL: $dir produced the wrong answer — staged the wrong object?"
        cat run.log
        exit 1
    }
}

make_project both   both   yes
make_project onlya  onlya  no

# ── direction A: the project with BOTH libraries runs first ──────────────────
# It disambiguates (two compress.c in one build dir), so the entry is written
# with a nested layout; `onlya` then asks for the flat one.
rm -rf "$MCPP_HOME/build-cache"
check_project both  both  cold
check_project onlya onlya reuses

# ── direction B: the single-library project runs first (fully symmetric) ─────
# Now the entry is written flat and `both` asks for the nested one. Before the
# fix this direction failed just as hard, with the paths swapped.
rm -rf "$MCPP_HOME/build-cache"
check_project onlya onlya cold
check_project both  both  reuses

# ── the entry's own addresses must be package-internal ───────────────────────
# `cache verify` reports an obj address that escaped its entry — the offline
# form of the same invariant, so a recurrence is auditable without reproducing
# a two-project build.
cd "$TMP/both"
"$MCPP" cache verify > verify.log 2>&1 || {
    echo "FAIL: cache verify reported incomplete entries"
    cat verify.log
    exit 1
}

echo "OK"
