#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 212_cached_dep_std_is_ordered.sh — mcpp#405.
#
# A dependency whose modules `import std`, consumed by a project that does NOT,
# must build on a cache HIT as well as on a miss.
#
# On a miss the dependency is compiled locally and that puts `gcm.cache/std.gcm`
# in the graph as a real prerequisite of its compile edges. On a HIT those
# compile edges are replaced by stage edges, and the std stage edge — emitted
# earlier, outside the loop that fills `_mcpp_staged_cache` — is left with zero
# consumers. ninja never runs it, and the restored BMI reports
#
#     std: failed to read compiled module: No such file or directory
#     <dep>: failed to read compiled module: Bad import dependency
#
# which names neither the cache nor std's absence from the graph.
#
# WHY THE FIRST PROJECT ALWAYS WORKED: miss and hit are different code paths.
# "It used to work and now it doesn't" is what a per-package cache turning warm
# looks like from the outside; the version never mattered.
#
# TWO THINGS THIS TEST MUST NOT DO
#
#  * It must NOT delete build outputs between builds. A missing artifact makes
#    ninja fail in the shape of a stale graph, the fast path falls back to a
#    full prepare, and the defect is covered — the unfixed binary goes green.
#  * It must NOT assert only on the exit code. A machine that happens to have
#    `gcm.cache/std.gcm` already staged passes for the wrong reason, so the
#    graph itself is asserted too.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/local-index"
INDEX_DIR_HOST="$(host_path "$INDEX_DIR")"
mkdir -p "$INDEX_DIR/pkgs/s"
cat > "$INDEX_DIR/pkgs/s/stdlib-dep.lua" <<'EOF'
package = {
    spec = "1",
    name = "stdlib-dep",
    description = "A dependency whose module imports std",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/stdlib-dep-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = true,
        sources = { "src/**/*.cppm" },
        targets = { ["stdlib-dep"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

# `make_project <dir> <name>` — the consumer deliberately does NOT `import std`.
# That one line is the whole reproduction: with it, the consumer's own compile
# edge names gcm.cache/std.gcm and the stage edge acquires a consumer.
make_project() {
    local dir="$1" name="$2"
    mkdir -p "$TMP/$dir/src"
    mkdir -p "$TMP/$dir/.mcpp/.xlings/data/xpkgs/local-dev.stdlib-dep/1.0.0/src"
    cat > "$TMP/$dir/.mcpp/.xlings/data/xpkgs/local-dev.stdlib-dep/1.0.0/src/lib.cppm" <<'EOF'
export module stdlib.dep;
import std;
export std::string dep_greeting() { return std::string("42"); }
EOF
    cat > "$TMP/$dir/src/main.cpp" <<'EOF'
import stdlib.dep;
extern "C" int puts(const char*);
int main() { puts(dep_greeting().c_str()); return 0; }
EOF
    cat > "$TMP/$dir/mcpp.toml" <<EOF
[package]
name = "$name"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR_HOST" }

[dependencies]
"local-dev.stdlib-dep" = "1.0.0"

[targets.$name]
kind = "bin"
main = "src/main.cpp"
EOF
}

find_ninja() { find "$1/target" -name build.ninja | head -1; }

# ── project one: cache MISS. Has always worked; asserted so a failure here is
#    read as "the fixture is wrong", not as the defect. ────────────────────────
make_project projmiss projmiss
cd "$TMP/projmiss"
"$MCPP" build > build.log 2>&1 || {
    echo "FAIL: cold build (cache miss) failed — fixture problem, not #405"
    cat build.log
    exit 1
}

# ── project two: cache HIT. This is the one that used to fail. ───────────────
make_project projhit projhit
cd "$TMP/projhit"
if ! "$MCPP" build > build.log 2>&1; then
    echo "FAIL: second project (cache hit) did not build"
    if grep -q 'Bad import dependency\|std.gcm' build.log; then
        echo "      this is mcpp#405: the restored BMI's std edge is not in the graph"
    fi
    cat build.log
    exit 1
fi

N="$(find_ninja "$TMP/projhit")"
[[ -n "$N" ]] || { echo "FAIL: projhit has no build.ninja"; exit 1; }

# The hit actually happened — otherwise the assertion below proves nothing.
grep -qE 'Cached local-dev\.stdlib-dep v1\.0\.0 \([0-9]+ unit' build.log || {
    echo "FAIL: the second project did not hit the cache, so #405 was not exercised"
    cat build.log
    exit 1
}

# THE graph assertion. The std BMI stage edge must be reachable, and the
# aggregate every non-staged edge already depends on is where it becomes so.
phony="$(grep -E '^build _mcpp_staged_cache : phony' "$N" || true)"
[[ -n "$phony" ]] || {
    echo "FAIL: no _mcpp_staged_cache aggregate, so the dependency was not staged"
    grep -n 'stdlib-dep' "$N" | head
    exit 1
}
case "$phony" in
    *std.gcm*) ;;
    *)
        echo "FAIL: the staged-cache aggregate does not include the std BMI"
        echo "      $phony"
        echo "      nothing else names gcm.cache/std.gcm, so ninja will never stage it"
        exit 1
        ;;
esac

# And it has to run. Nothing is deleted anywhere in this file.
./target/*/*/bin/projhit > run.log 2>&1 || { cat run.log; exit 1; }
grep -q '^42$' run.log || {
    echo "FAIL: staged artifacts produced wrong output"
    cat run.log
    exit 1
}

echo "PASS: a cached dependency's transitive std BMI is staged for a consumer that does not import std"
