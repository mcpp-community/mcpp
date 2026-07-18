#!/usr/bin/env bash
# requires: elf gcc
# mcpp#239 (follow-up to #233): a dependency's build.mcpp can emit generated
# sources into OUT_DIR (<consumer>/target/.build-mcpp/deps/<name>@<ver>/out),
# which live OUTSIDE the dependency's package root. Their scanner relPath is
# `relative(<abs OUT_DIR src>, <dep root>)` and therefore carries `..`
# components. #233's collision-disambiguation pasted that relPath straight
# into the object path:
#   cc1plus: fatal error: opening output file
#     obj/<dep>/../<consumer>/target/.../out/gen.o: No such file or directory
# i.e. `obj/<dep>/../…` climbs out of obj/ (and with enough `..`, mirrors the
# whole absolute tree under CWD). The collision prefix must be sanitized so
# every object stays under obj/.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# Dependency whose build.mcpp writes gen.cpp into OUT_DIR (absolute path,
# outside the package root).
mkdir -p gdep/src
cat > gdep/build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    std::string p = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::FILE* f = std::fopen(p.c_str(), "w");
    std::fputs("int gen_val() { return 7; }\n", f);
    std::fclose(f);
    mcpp::generated("gen.cpp");
    return 0;
}
EOF
cat > gdep/src/lib.cpp <<'EOF'
extern int gen_val();
int gdep_use() { return gen_val(); }
EOF
cat > gdep/mcpp.toml <<'EOF'
[package]
name    = "gdep"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[targets.gdep]
kind = "lib"
EOF

# Consumer with its OWN src/gen.cpp — same basename as gdep's generated
# gen.cpp, forcing the disambiguation branch on the out-of-root abs source.
mkdir -p consumer/src
cat > consumer/src/gen.cpp <<'EOF'
int consumer_gen() { return 100; }
EOF
cat > consumer/src/main.cpp <<'EOF'
import std;
extern int gdep_use();
extern int consumer_gen();
int main() {
    std::println("sum={}", gdep_use() + consumer_gen());
    return (gdep_use() == 7 && consumer_gen() == 100) ? 0 : 1;
}
EOF
cat > consumer/mcpp.toml <<'EOF'
[package]
name    = "consumer"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
gdep = { path = "../gdep" }
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF

cd consumer
"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "FAIL: build failed (expected: out-of-root generated source must map under obj/)"
    exit 1
}

ninja_file="$(find target -name build.ninja | head -1)"
[[ -n "$ninja_file" ]] || { echo "no build.ninja generated"; exit 1; }

# No object output may escape obj/ via `..` or an absolute root.
bad="$(grep -oE 'build [^ ]+\.o : cxx_object' "$ninja_file" | awk '{print $2}' \
        | grep -E '(^|/)\.\.(/|$)|^/' || true)"
if [[ -n "$bad" ]]; then
    echo "FAIL: object path escapes obj/:"
    echo "$bad"
    exit 1
fi

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "sum=107" ]] || { echo "unexpected output: $out"; exit 1; }

echo "OK"
