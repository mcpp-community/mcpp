#!/usr/bin/env bash
# requires: gcc
# A RANGE IN THE VERSION POSITION IS A CONSTRAINT, AND IT MUST BE ANSWERABLE.
#
# The version position of an xlings address has accepted range expressions all
# along and xlings resolves one when it installs. `xpkg_dir` did not: it treated
# the whole position as a directory name, so `>=0.9.9` installed a payload and
# then reported that nothing was installed. A rule package could therefore state
# a floor and could not then find what the floor had brought in -- which is why
# every project using a rule repeated the rule's own package list with exact
# pins.
#
# THIS IS THE ARRANGEMENT THE RANGE EXISTS FOR: the rule owns "which package,
# and no older than what", the project owns "and exactly this one" -- and writes
# nothing at all when it has no opinion, which is the common case.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

TOOL=zoxide

mkdir -p dep/src
cat > dep/src/lib.cpp <<'EOF'
int dep_touch() { return 1; }
EOF
cat > dep/build.mcpp <<EOF
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* d = mcpp::xpkg_dir("xim", "$TOOL");
    if (d == nullptr || d[0] == '\0') {
        std::fprintf(stderr,
            "a [feature-xlings] entry stated as a range was installed and then "
            "answered as absent\n");
        return 2;
    }
    std::string out = std::string(mcpp::manifest_dir()) + "/answered.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "%s\n", d);
    std::fclose(f);
    return 0;
}
EOF
# UNDER A FEATURE, which is the shape a rule package uses: the tool arrives only
# for the consumers that asked for the rule.
cat > dep/mcpp.toml <<EOF
[package]
name    = "toolowner"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[features]
default  = []
usestool = []
[feature-xlings.usestool]
"xim:$TOOL" = ">=0.9.9"
[targets.toolowner]
kind = "lib"
EOF

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
# THE PROJECT DECLARES NO TOOL AT ALL. That is the whole claim: one edge, and
# the environment the rule needs comes with it.
cat > app/mcpp.toml <<'EOF'
[package]
name    = "consumer"
version = "0.1.0"
[dependencies]
toolowner = { path = "../dep", features = ["usestool"] }
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF

export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"

cd app
if ! "$MCPP" build >build.log 2>&1; then
    echo "FAIL: the consumer did not build"
    grep -iE 'answered as absent|error' build.log | head -5
    tail -10 build.log
    exit 1
fi

answered="$TMP/dep/answered.txt"
[ -s "$answered" ] || {
    echo "FAIL: the build program recorded nothing"
    tail -10 build.log
    exit 1
}
echo "xpkg_dir answered: $(cat "$answered")"
grep -q "xim-x-$TOOL" "$answered" || {
    echo "FAIL: the recorded path does not name $TOOL: $(cat "$answered")"
    exit 1
}
# THE HIGHEST INSTALLED VERSION SATISFYING THE RANGE, not merely some version.
# 0.9.7 is in the index and does not satisfy `>=0.9.9`; answering it would be a
# floor silently lowered.
grep -qE '/(0\.9\.9|0\.10\.[0-9]+)$' "$answered" || {
    echo "FAIL: the answer does not satisfy >=0.9.9: $(cat "$answered")"
    exit 1
}

echo "PASS: a range is installed and answered, and the project declared no tool"
