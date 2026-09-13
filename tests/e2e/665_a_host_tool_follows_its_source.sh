#!/usr/bin/env bash
# requires: gcc
# 665 -- a cached host tool follows its source (#630, item 6). The tool store
# was keyed on the package's version, so a change to a `path` tool's source
# under an unchanged version was invisible to every consumer until the version
# moved; the build reported success over the previous compiler's output. The
# key now carries a stamp of the tree for a `path` package and the resolved
# commit for a `git` package. Measured in both directions, at one path and
# with different bytes: an edit reaches the consumer, its reversal reaches it
# too, and a tree that did not change is not rebuilt (e2e 187's claim, which
# this test keeps).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# An isolated store, so that an entry from a previous run cannot decide.
export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
if [ -d "$HOME/.mcpp/registry" ]; then
    ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fi

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

write_tool() {   # $1 = directory, $2 = the answer the generator writes
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<'EOF'
[package]
name    = "toolpkg"
version = "0.1.0"

[targets.gen]
kind = "bin"
main = "src/gen.cpp"
EOF
    cat > "$1/src/gen.cpp" <<EOF
#include <cstdio>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int generated_answer() { return $2; }\n");
    std::fclose(f);
    return 0;
}
EOF
}

write_app() {    # $1 = directory, $2 = the dependency table line
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies]
$2
EOF
    cat > "$1/src/main.cpp" <<'EOF'
#include <cstdio>
int generated_answer();
int main() { std::printf("ANSWER=%d\n", generated_answer()); }
EOF
    cat > "$1/build.mcpp" <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    const char* tool = mcpp::dep_bin("toolpkg", "gen");
    if (!tool || !*tool) { std::fprintf(stderr, "no tool path\n"); return 1; }
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::string cmd = std::string("\"") + tool + "\" \"" + out + "\"";
    if (std::system(cmd.c_str()) != 0) { std::fprintf(stderr, "tool failed\n"); return 1; }
    mcpp::generated(out.c_str());
}
EOF
}

answer() {       # builds in the current directory and prints the program's answer
    rm -rf target
    "$MCPP" build > "$1" 2>&1 || fail "build failed" "$1"
    "$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1
}

# ── A `path` tool: same version, different bytes, both directions ────────────
write_tool toolpkg 41
write_app app 'toolpkg = { path = "../toolpkg", tools = ["gen"] }'
cd app
[ "$(answer b1.log)" = "ANSWER=41" ] || fail "first build: expected 41" b1.log
grep -q "Building.*host tool" b1.log || fail "the first build did not build the tool" b1.log

# Unchanged tree: a store hit, no rebuild. (The rebuild line is the claim
# e2e 187 makes; a stamp that moved without a change would break it.)
sleep 1
[ "$(answer b2.log)" = "ANSWER=41" ] || fail "second build: expected 41" b2.log
grep -q "Building.*host tool" b2.log && fail "an unchanged path tool was rebuilt" b2.log
echo "ok: an unchanged path tool is a store hit"

# The edit, with the version untouched.
sleep 1
write_tool ../toolpkg 42
[ "$(answer b3.log)" = "ANSWER=42" ] || fail "after the edit: expected 42" b3.log
grep -q "Building.*host tool" b3.log || fail "the edit did not rebuild the tool" b3.log
echo "ok: an edit to a path tool reaches the consumer without a version bump"

# And its reversal, which a key that only ever grew would miss.
sleep 1
write_tool ../toolpkg 41
[ "$(answer b4.log)" = "ANSWER=41" ] || fail "after the reversal: expected 41" b4.log
grep -q "Building.*host tool" b4.log || fail "the reversal did not rebuild the tool" b4.log
echo "ok: the reversal reaches the consumer too"

# ── A `git` tool: keyed by the resolved commit ──────────────────────────────
cd "$TMP"
write_tool gittool 7
git -C gittool init -q -b main
git -C gittool -c user.name=t -c user.email=t@t add -A
git -C gittool -c user.name=t -c user.email=t@t commit -qm "seven"
A=$(git -C gittool rev-parse HEAD)
write_tool gittool 8
git -C gittool -c user.name=t -c user.email=t@t commit -qam "eight"
B=$(git -C gittool rev-parse HEAD)

write_app gapp "toolpkg = { git = \"file://$TMP/gittool\", rev = \"$A\", tools = [\"gen\"] }"
cd gapp
[ "$(answer g1.log)" = "ANSWER=7" ] || fail "git rev A: expected 7" g1.log
sed -i "s/$A/$B/" mcpp.toml
[ "$(answer g2.log)" = "ANSWER=8" ] || fail "git rev B: expected 8" g2.log
grep -q "Building.*host tool" g2.log || fail "moving the pin to B did not rebuild the tool" g2.log
# Back to A: the entry built for A is still valid, so nothing is rebuilt.
sed -i "s/$B/$A/" mcpp.toml
[ "$(answer g3.log)" = "ANSWER=7" ] || fail "git rev A again: expected 7" g3.log
grep -q "Building.*host tool" g3.log && fail "the entry for commit A was not reused" g3.log
echo "ok: a git tool is keyed by its commit, and each commit's entry is reused"
