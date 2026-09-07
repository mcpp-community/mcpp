#!/usr/bin/env bash
# requires: gcc
# ONE PACKAGE, ONE VERSION.
#
# A project and one of its dependencies both name `xim:zoxide`, at different
# versions. mcpp had two definitions of when two addresses name one package: the
# conditional merge compared the PACKAGE, the graph split compared the whole
# STRING. Under the second, these were two packages -- both installed, and the
# `xpkg_dir` answer decided by whichever "keep the first value for a name" rule
# a later pass happened to reach. Installed twice, answered once, said nothing.
#
# THE CRITERION IS BOTH HALVES AT ONCE. Asserting only "one directory exists"
# would pass on an engine that installs one and answers the other; asserting
# only the answer would pass on one that installs two. The pair is what
# identifies the defect.
#
# WHY zoxide: three versions in the index for all three host platforms, about a
# megabyte each. The tool is never run -- only its payload directory is asked
# about -- so the test measures resolution, not the tool.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

TOOL=zoxide
PROJECT_PIN=0.9.9
DEP_PIN=0.9.7

mkdir -p dep/src
cat > dep/src/lib.cpp <<'EOF'
int dep_touch() { return 1; }
EOF
# THE ANSWER IS RECORDED BY THE DEPENDENCY'S OWN BUILD PROGRAM, which is the
# reader that has to agree with the installer. To a file, because mcpp prints a
# build program's output only when it fails.
cat > dep/build.mcpp <<EOF
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* d = mcpp::xpkg_dir("xim", "$TOOL");
    std::string out = std::string(mcpp::manifest_dir()) + "/answered.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "%s\n", d == nullptr ? "" : d);
    std::fclose(f);
    return 0;
}
EOF
cat > dep/mcpp.toml <<EOF
[package]
name    = "toolowner"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[xlings.workspace]
"xim:$TOOL" = "$DEP_PIN"
[targets.toolowner]
kind = "lib"
EOF

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > app/mcpp.toml <<EOF
[package]
name    = "consumer"
version = "0.1.0"
[dependencies]
toolowner = { path = "../dep" }
[xlings.workspace]
"xim:$TOOL" = "$PROJECT_PIN"
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF

# AN ISOLATED HOME. The ambient registry may already hold either version, and
# then the store count says nothing.
export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"

cd app
if ! "$MCPP" build >build.log 2>&1; then
    echo "FAIL: the consumer did not build"
    tail -20 build.log
    exit 1
fi

STORE="$MCPP_HOME/registry/data/xpkgs/xim-x-$TOOL"
[ -d "$STORE" ] || { echo "FAIL: $TOOL was never installed ($STORE)"; tail -20 build.log; exit 1; }
installed=$(ls -1 "$STORE" | sort | tr '\n' ' ')
count=$(ls -1 "$STORE" | wc -l | tr -d ' ')
echo "installed versions: $installed"
[ "$count" -eq 1 ] || {
    echo "FAIL: two declarations of one package installed $count versions: $installed"
    exit 1
}
[ -d "$STORE/$PROJECT_PIN" ] || {
    echo "FAIL: the declaration nearer the artifact did not win: $installed"
    exit 1
}

answered="$TMP/dep/answered.txt"
[ -s "$answered" ] || { echo "FAIL: the dependency's build program recorded nothing"; exit 1; }
echo "xpkg_dir answered: $(cat "$answered")"
grep -q "$PROJECT_PIN" "$answered" || {
    echo "FAIL: installed $PROJECT_PIN and answered $(cat "$answered")"
    exit 1
}

# AND IT IS SAID OUT LOUD. An override visible only as "two versions were
# declared and one directory exists" is a fact the reader has to reconstruct
# from the filesystem.
grep -q "declared at two versions" build.log || {
    echo "FAIL: the override was not reported"
    grep -i 'zoxide' build.log | head -5
    exit 1
}
grep -q "toolowner" build.log || {
    echo "FAIL: the report does not name the dependency that lost"
    exit 1
}

echo "PASS: one package, one version -- installed, answered and reported"
