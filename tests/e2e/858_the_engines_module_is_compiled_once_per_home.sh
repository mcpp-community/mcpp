#!/usr/bin/env bash
# requires:
# 858 -- the engine interface (`mcpp.core`) is compiled once per mcpp version and
# host compiler, for every project that shares a home.
#
# #748 (B1), mcpp#797. The interface is the engine's own text, written as a
# package under $MCPP_HOME/engine/<version>/modules, and the build programs'
# graph depends on it like on any library. Its compiled entry is kept in the
# global cache under `_engine/mcpp-build-module@<version>+<digest>`, so `mcpp cache list`,
# `info`, `verify` and `gc` treat it as they treat a dependency's entry.
#
# Criteria:
#   A. The first project's build programs' graph compiles the interface; a
#      second project with another name in another directory, sharing the home,
#      places the cached entry and compiles nothing of it.
#   B. The entry is one of `mcpp cache list`, is complete to `mcpp cache verify`,
#      and records inputs that name no project: the flags it was compiled with
#      are the toolchain's, so they hold for every project.
#   C. `mcpp cache gc` collects it, and the next build compiles it again.
#   D. `--cache local` writes nothing below the global cache's package tree; the
#      interface is compiled in the project's build programs' graph.
#   E. A library of an index package that a build program imports is kept in
#      the global cache too, under its package address, and a second project
#      that imports it compiles nothing of it.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

project() {
    local dir="$1" name="$2"
    mkdir -p "$dir/src"
    cat > "$dir/mcpp.toml" <<EOF
[package]
name    = "$name"
version = "0.1.0"

[targets.$name]
kind = "bin"
main = "src/main.cpp"
EOF
    printf 'int main() { return 0; }\n' > "$dir/src/main.cpp"
    cat > "$dir/build.mcpp" <<EOF
import mcpp;
int main() { mcpp::define("T858_${name}=1"); return 0; }
EOF
}
project "$TMP/alpha" alpha858
project "$TMP/beta"  beta858
ALPHA_HOST="$(host_path "$TMP/alpha")"
BETA_HOST="$(host_path "$TMP/beta")"

# The build programs' graph of project $1, and how it obtains object $2.
host_ninja() { find "$1/target/.build-mcpp/host" -name build.ninja | head -1; }
compiles() { grep -qE "^build obj/[^ ]*/$2( [|] [^:]*)? : cxx_module " "$(host_ninja "$1")"; }
places()   { grep -qE "^build obj/[^ ]*/$2 : stage_file [^ ]*/build-cache/v1/pkg/$3" "$(host_ninja "$1")"; }

# A
( cd "$TMP/alpha" && MCPP_VERBOSE=1 "$MCPP" build > "$TMP/alpha.log" 2>&1 ) \
    || fail "the first project did not build" "$TMP/alpha.log"
compiles "$TMP/alpha" 'mcpp\.m\.o' \
    || fail "A: the first project did not compile the engine interface" "$TMP/alpha.log" "$(host_ninja "$TMP/alpha")"
( cd "$TMP/beta" && MCPP_VERBOSE=1 "$MCPP" build > "$TMP/beta.log" 2>&1 ) \
    || fail "the second project did not build" "$TMP/beta.log"
places "$TMP/beta" 'mcpp\.m\.o' '_engine/mcpp-build-module@' \
    || fail "A: the second project did not place the cached entry" "$TMP/beta.log" "$(host_ninja "$TMP/beta")"
compiles "$TMP/beta" 'mcpp\.m\.o' && fail "A: the second project compiled the engine interface again" "$(host_ninja "$TMP/beta")"

# B
"$MCPP" cache list > "$TMP/list.log" 2>&1
[ "$(grep -c '_engine/mcpp-build-module@' "$TMP/list.log")" = 1 ] \
    || fail "B: expected one entry of the bundled module in the cache" "$TMP/list.log"
"$MCPP" cache verify > "$TMP/verify.log" 2>&1 || fail "B: mcpp cache verify failed" "$TMP/verify.log"
grep -q "all complete" "$TMP/verify.log" || fail "B: the entry is not complete" "$TMP/verify.log"
"$MCPP" cache info _engine/mcpp-build-module > "$TMP/info.log" 2>&1
grep -q 'mcpp.cppm' "$TMP/info.log" || fail "B: the entry does not record its sources" "$TMP/info.log"
if grep -qF "$ALPHA_HOST" "$TMP/info.log" || grep -qF "$BETA_HOST" "$TMP/info.log"; then
    fail "B: the recorded inputs name a project, so the entry could not serve another" "$TMP/info.log"
fi

# C
sleep 2
"$MCPP" cache gc --older-than 1s > "$TMP/gc.log" 2>&1 || fail "C: mcpp cache gc failed" "$TMP/gc.log"
grep -q "Collected 1 entries" "$TMP/gc.log" || fail "C: gc did not collect the entry of the bundled module" "$TMP/gc.log"
( cd "$TMP/alpha" && touch build.mcpp && echo '// edited' >> build.mcpp \
    && MCPP_VERBOSE=1 "$MCPP" build > "$TMP/alpha2.log" 2>&1 ) \
    || fail "C: the build after gc failed" "$TMP/alpha2.log"
compiles "$TMP/alpha" 'mcpp\.m\.o' \
    || fail "C: the interface was not compiled again after gc collected it" "$TMP/alpha2.log"

# D
export MCPP_HOME="$TMP/mcpp-home-local"
source "$(dirname "$0")/_inherit_toolchain.sh"
project "$TMP/gamma" gamma858
( cd "$TMP/gamma" && MCPP_VERBOSE=1 "$MCPP" build --cache local > "$TMP/gamma.log" 2>&1 ) \
    || fail "D: the --cache local build failed" "$TMP/gamma.log"
if [ -n "$(find "$MCPP_HOME/build-cache/v1/pkg" -name entry.json 2>/dev/null)" ]; then
    fail "D: --cache local wrote an entry below the global cache's package tree" "$TMP/gamma.log"
fi
compiles "$TMP/gamma" 'mcpp\.m\.o' \
    || fail "D: the interface was not compiled in the project's build programs' graph" "$TMP/gamma.log"

# E. The package is in a local index, and its payload is where the store keeps
# installed packages: the two facts the cache admits a package on.
export MCPP_HOME="$TMP/mcpp-home-index"
source "$(dirname "$0")/_inherit_toolchain.sh"
INDEX_DIR="$TMP/local-index"
INDEX_DIR_HOST="$(host_path "$INDEX_DIR")"
mkdir -p "$INDEX_DIR/pkgs/r"
cat > "$INDEX_DIR/pkgs/r/rule-dep.lua" <<'LUA'
package = {
    spec = "1",
    name = "rule-dep",
    description = "A library a build program imports",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = { ["1.0.0"] = {
            url = "https://example.invalid/rule-dep-1.0.0.tar.gz",
            sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        macosx = { ["1.0.0"] = {
            url = "https://example.invalid/rule-dep-1.0.0.tar.gz",
            sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        windows = { ["1.0.0"] = {
            url = "https://example.invalid/rule-dep-1.0.0.tar.gz",
            sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
    },
}
LUA
indexed() {
    local dir="$1" name="$2"
    local payload="$dir/.mcpp/.xlings/data/xpkgs/local-dev.rule-dep/1.0.0"
    mkdir -p "$dir/src" "$payload/src"
    cat > "$payload/mcpp.toml" <<'TOML'
[package]
name    = "rule-dep"
version = "1.0.0"

[targets.rule-dep]
kind = "lib"
TOML
    cat > "$payload/src/rule-dep.cppm" <<'CPPM'
export module t858.rule;
import mcpp;
export int rule_v() { return 5; }
CPPM
    cat > "$dir/mcpp.toml" <<MANIFEST
[package]
name    = "$name"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR_HOST" }

[build-dependencies]
"local-dev.rule-dep" = { version = "1.0.0", host-module = true }

[targets.$name]
kind = "bin"
main = "src/main.cpp"
MANIFEST
    printf 'int main() { return 0; }\n' > "$dir/src/main.cpp"
    cat > "$dir/build.mcpp" <<'BUILDMCPP'
import mcpp;
import t858.rule;
int main() { return rule_v() == 5 ? 0 : 1; }
BUILDMCPP
}
indexed "$TMP/delta" delta858
indexed "$TMP/epsilon" epsilon858
( cd "$TMP/delta" && MCPP_VERBOSE=1 "$MCPP" build > "$TMP/delta.log" 2>&1 ) \
    || fail "E: the first project that imports the index package's library did not build" "$TMP/delta.log"
compiles "$TMP/delta" 'rule-dep\.m\.o' \
    || fail "E: the first project did not compile the library" "$TMP/delta.log" "$(host_ninja "$TMP/delta")"
entry=$(find "$MCPP_HOME/build-cache/v1/pkg" -path '*local-dev.rule-dep@1.0.0*' -name entry.json 2>/dev/null | head -1)
[ -n "$entry" ] || fail "E: the library of an index package is not in the global cache" "$TMP/delta.log"
case "$entry" in
    */pkg/local-dev/local-dev.rule-dep@1.0.0/*/entry.json) ;;
    *) fail "E: the entry is not at its package address: $entry" ;;
esac
( cd "$TMP/epsilon" && MCPP_VERBOSE=1 "$MCPP" build > "$TMP/epsilon.log" 2>&1 ) \
    || fail "E: the second project did not build" "$TMP/epsilon.log"
places "$TMP/epsilon" 'rule-dep\.m\.o' 'local-dev/local-dev.rule-dep@1.0.0' \
    || fail "E: the second project did not place the cached library" "$TMP/epsilon.log" "$(host_ninja "$TMP/epsilon")"

echo "PASS: 858_the_engines_module_is_compiled_once_per_home"
