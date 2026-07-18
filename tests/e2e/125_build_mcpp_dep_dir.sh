#!/usr/bin/env bash
# requires: elf gcc
# mcpp#241: build.mcpp env contract exposes each resolved dependency's install
# dir as MCPP_DEP_<SANITIZED_NAME>_DIR (read via mcpp::dep_dir("<name>")), so a
# package's build.mcpp can locate a dependency's payload (e.g. a data-asset
# package) by name instead of reverse-engineering the store layout. Scenario:
# consumer -> pkgp (has build.mcpp) -> datad (data package). pkgp's build.mcpp
# reads dep_dir("datad") and generates a source returning it; consumer prints it.
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

# middle package with a build.mcpp that locates datad's dir
mkdir -p pkgp/src
cat > pkgp/src/lib.cpp <<'EOF'
extern const char* dep_datad_dir();
const char* pkgp_datad_dir() { return dep_datad_dir(); }
EOF
cat > pkgp/build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* d = mcpp::dep_dir("datad");
    if (d == nullptr || d[0] == '\0') {
        std::fprintf(stderr, "build.mcpp: MCPP_DEP_DATAD_DIR not set\n");
        return 1;   // fail loudly if the contract var is missing
    }
    std::string out = std::string(mcpp::out_dir()) + "/depdir.cpp";
    std::FILE* f = std::fopen(out.c_str(), "w");
    std::fprintf(f, "extern const char* dep_datad_dir();\n");
    std::fprintf(f, "const char* dep_datad_dir() { return \"%s\"; }\n", d);
    std::fclose(f);
    mcpp::generated("depdir.cpp");
    return 0;
}
EOF
cat > pkgp/mcpp.toml <<'EOF'
[package]
name    = "pkgp"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
datad = { path = "../datad" }
[targets.pkgp]
kind = "lib"
EOF

# root consumer
mkdir -p consumer/src
cat > consumer/src/main.cpp <<'EOF'
import std;
extern const char* pkgp_datad_dir();
int main() {
    std::println("DEPDIR={}", pkgp_datad_dir());
    return 0;
}
EOF
cat > consumer/mcpp.toml <<'EOF'
[package]
name    = "consumer"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
pkgp = { path = "../pkgp" }
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF

cd consumer
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "FAIL: build failed"; exit 1; }

out="$("$MCPP" run 2>&1 | grep '^DEPDIR=' | tail -1)"
dir="${out#DEPDIR=}"
[[ -n "$dir" ]] || { echo "FAIL: empty dep dir (contract var missing)"; exit 1; }
[[ -d "$dir" ]] || { echo "FAIL: dep dir does not exist: $dir"; exit 1; }
[[ "$(basename "$dir")" == "datad" ]] || { echo "FAIL: dep dir is not datad's root: $dir"; exit 1; }

echo "OK"
