#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 172_build_cache_cross_project.sh — the global build cache must actually save
# work, and it must save it ACROSS projects.
#
# Two defects met here, and this file is the gate for both.
#
#  1. The key was the whole-project fingerprint, which serializes every package
#     in the graph INCLUDING the root — its name, its version, its [build]
#     flags. So two projects never shared an entry, and bumping a project's own
#     version invalidated every dependency it had. (Measured on one machine:
#     26 GB across 1198 fingerprint directories, `compat.zlib@1.3.2` stored 162
#     times.)
#
#  2. Even on a hit, nothing was saved. Artifacts were copied into the build dir
#     from inside prepare_build while those paths stayed declared as compile edge
#     outputs — and ninja treats an output it has no command line for in
#     .ninja_log as dirty ("command line not found in log"), which a fresh build
#     dir always is. Every "cached" unit was recompiled while the CLI printed
#     "Cached".
#
# So the load-bearing assertion is not "an entry exists" but "the second project
# has ZERO compile edges for the dependency's sources". Read from build.ninja,
# not from the log: a status line is exactly what lied before.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

# ── an offline path index serving one library package ────────────────────────
INDEX_DIR="$TMP/local-index"
mkdir -p "$INDEX_DIR/pkgs/s"
cat > "$INDEX_DIR/pkgs/s/shared-lib.lua" <<'EOF'
package = {
    spec = "1",
    name = "shared-lib",
    description = "Shared across projects",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/shared-lib-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = true,
        sources = { "src/**/*.cppm" },
        targets = { ["shared-lib"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

# `make_project <dir> <package-name> <package-version>` — same dependency, and
# deliberately DIFFERENT identity, because the consumer's identity is exactly
# what used to leak into the dependency's cache key.
make_project() {
    local dir="$1" name="$2" version="$3"
    mkdir -p "$TMP/$dir/src"
    mkdir -p "$TMP/$dir/.mcpp/.xlings/data/xpkgs/local-dev.shared-lib/1.0.0/src"
    cat > "$TMP/$dir/.mcpp/.xlings/data/xpkgs/local-dev.shared-lib/1.0.0/src/lib.cppm" <<'EOF'
export module shared.lib;
export int shared_value() { return 41; }
EOF
    cat > "$TMP/$dir/src/main.cpp" <<'EOF'
import std;
import shared.lib;
int main() { std::println("{}", shared_value() + 1); return 0; }
EOF
    cat > "$TMP/$dir/mcpp.toml" <<EOF
[package]
name = "$name"
version = "$version"

[indices]
local-dev = { path = "$INDEX_DIR" }

[dependencies]
"local-dev.shared-lib" = "1.0.0"

[targets.$name]
kind = "bin"
main = "src/main.cpp"
EOF
}

dep_compile_edges() {   # $1 = build.ninja
    grep -cE ': (cxx_module|cxx_object|cxx_scan) .*shared-lib' "$1" || true
}
dep_stage_edges() {     # $1 = build.ninja
    grep -cE '^build .* : stage_file .*shared-lib' "$1" || true
}
find_ninja() { find "$1/target" -name build.ninja | head -1; }

# ── project one: cold, must compile the dependency and populate the cache ────
make_project projone projone 0.1.0
cd "$TMP/projone"
"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }

N1="$(find_ninja "$TMP/projone")"
[[ -n "$N1" ]] || { echo "FAIL: projone has no build.ninja"; exit 1; }
[[ "$(dep_compile_edges "$N1")" -gt 0 ]] || {
    echo "FAIL: cold build had no compile edges for the dependency"
    grep -n 'shared-lib' "$N1" | head
    exit 1
}
"$MCPP" cache list > list1.log 2>&1
grep -q 'shared-lib' list1.log || {
    echo "FAIL: cold build did not populate a cache entry"
    cat list1.log; cat build.log
    exit 1
}

# ── project two: DIFFERENT name and version, same dependency ─────────────────
make_project projtwo projtwo 9.9.9
cd "$TMP/projtwo"
"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }

N2="$(find_ninja "$TMP/projtwo")"
[[ -n "$N2" ]] || { echo "FAIL: projtwo has no build.ninja"; exit 1; }

# THE assertion.
edges="$(dep_compile_edges "$N2")"
if [[ "$edges" != "0" ]]; then
    echo "FAIL: second project recompiled the dependency ($edges compile edges)"
    echo "      the cache key is still carrying the consumer's identity"
    grep -nE ': (cxx_module|cxx_object|cxx_scan) .*shared-lib' "$N2" | head
    exit 1
fi
staged="$(dep_stage_edges "$N2")"
[[ "$staged" -gt 0 ]] || {
    echo "FAIL: dependency has neither compile nor stage edges — it is missing"
    grep -n 'shared-lib' "$N2" | head
    exit 1
}

# The status line must agree, and must carry the unit count. The bare word
# "Cached" was printed for months while every unit was recompiled behind it; a
# number that has to match the skipped edges cannot go quietly wrong that way.
grep -qE 'Cached local-dev\.shared-lib v1\.0\.0 \([0-9]+ unit' build.log || {
    echo "FAIL: no 'Cached ... (N units)' line for the reused dependency"
    cat build.log
    exit 1
}

# And it has to WORK: staged artifacts must link and run.
./target/*/*/bin/projtwo > run.log 2>&1 || { cat run.log; exit 1; }
grep -q '^42$' run.log || { echo "FAIL: staged artifacts produced wrong output"; cat run.log; exit 1; }

# ── the consumer's own version must not invalidate the dependency ────────────
cd "$TMP/projone"
sed -i.bak 's/version = "0.1.0"/version = "0.2.0"/' mcpp.toml
rm -f mcpp.toml.bak
rm -rf target
"$MCPP" build > bump.log 2>&1 || { cat bump.log; exit 1; }
N3="$(find_ninja "$TMP/projone")"
[[ "$(dep_compile_edges "$N3")" == "0" ]] || {
    echo "FAIL: bumping the consumer's own version invalidated the dependency"
    grep -nE ': (cxx_module|cxx_object|cxx_scan) .*shared-lib' "$N3" | head
    exit 1
}

# ── a real change to the dependency MUST invalidate it (no false hits) ───────
# Bump the dependency's version in the index and in the manifests: a different
# version is a different entry, so it has to be compiled again.
sed -i.bak 's/\["1\.0\.0"\]/["1.0.1"]/' "$INDEX_DIR/pkgs/s/shared-lib.lua"
rm -f "$INDEX_DIR/pkgs/s/shared-lib.lua.bak"
mv "$TMP/projone/.mcpp/.xlings/data/xpkgs/local-dev.shared-lib/1.0.0" \
   "$TMP/projone/.mcpp/.xlings/data/xpkgs/local-dev.shared-lib/1.0.1"
sed -i.bak 's/"local-dev.shared-lib" = "1.0.0"/"local-dev.shared-lib" = "1.0.1"/' \
    "$TMP/projone/mcpp.toml"
rm -f "$TMP/projone/mcpp.toml.bak"
rm -rf target
"$MCPP" build > newver.log 2>&1 || { cat newver.log; exit 1; }
N4="$(find_ninja "$TMP/projone")"
[[ "$(dep_compile_edges "$N4")" -gt 0 ]] || {
    echo "FAIL: a new dependency version was served from the old entry"
    cat newver.log
    exit 1
}

echo "OK"
