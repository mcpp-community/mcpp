#!/usr/bin/env bash
# requires: gcc
# P1 (large-source-pkg design §3.1 item 4): the ROOT project's build.mcpp now
# runs AFTER dependency resolution and receives MCPP_DEP_<NAME>_DIR exactly
# like a dependency's build.mcpp does (mcpp#241 contract, previously dep-only:
# the root ran before resolution and saw none). Scenario mirrors
# 125_build_mcpp_dep_dir.sh, but the build.mcpp asserting dep_dir() is the
# ROOT's own: root -> datad (path dep); root build.mcpp reads
# dep_dir("datad"), fails loudly if unset, and generates a source returning
# it; main prints it; the test asserts it is datad's real root.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# data-asset dependency
mkdir -p datad/src
echo 'int datad_touch() { return 1; }' > datad/src/d.cpp
cat > datad/mcpp.toml <<'EOF'
[package]
name    = "datad"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[targets.datad]
kind = "lib"
EOF

# ROOT project whose own build.mcpp locates datad's dir
mkdir -p rootp/src
cat > rootp/build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* d = mcpp::dep_dir("datad");
    if (d == nullptr || d[0] == '\0') {
        std::fprintf(stderr, "build.mcpp: MCPP_DEP_DATAD_DIR not set for the ROOT\n");
        return 1;   // the moved call point must expose the contract var
    }
    std::string out = "src/depdir.cpp";
    std::FILE* f = std::fopen(out.c_str(), "w");
    std::fprintf(f, "const char* root_datad_dir() { return \"%s\"; }\n", d);
    std::fclose(f);
    mcpp::generated("src/depdir.cpp");
    return 0;
}
EOF
cat > rootp/src/main.cpp <<'EOF'
import std;
extern const char* root_datad_dir();
int main() {
    std::println("DEPDIR={}", root_datad_dir());
    return 0;
}
EOF
cat > rootp/mcpp.toml <<'EOF'
[package]
name    = "rootp"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
datad = { path = "../datad" }
[targets.rootp]
kind = "bin"
main = "src/main.cpp"
EOF

cd rootp
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "FAIL: build failed"; exit 1; }

out="$("$MCPP" run 2>&1 | grep '^DEPDIR=' | tail -1)"
dir="${out#DEPDIR=}"
[[ -n "$dir" ]] || { echo "FAIL: empty dep dir (root contract var missing)"; exit 1; }
[[ -d "$dir" ]] || { echo "FAIL: dep dir does not exist: $dir"; exit 1; }
[[ "$(basename "$dir")" == "datad" ]] || { echo "FAIL: dep dir is not datad's root: $dir"; exit 1; }

echo "OK"
