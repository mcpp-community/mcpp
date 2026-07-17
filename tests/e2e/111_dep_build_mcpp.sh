#!/usr/bin/env bash
# requires: gcc
# G2: a dependency's build.mcpp runs (Cargo build.rs model) with Cargo scope:
# its cfg define reaches ONLY the dep's own TUs; its generated source (written
# to MCPP_OUT_DIR) compiles into the dep; artifacts live in the CONSUMING
# project's tree — the dep root itself is never written to.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/deplib/src" "$TMP/app/src"

cat > "$TMP/deplib/mcpp.toml" <<'EOF'
[package]
name    = "deplib"
version = "0.1.0"

[targets.deplib]
kind = "lib"
EOF

cat > "$TMP/deplib/build.mcpp" <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
int main() {
    const char* out = std::getenv("MCPP_OUT_DIR");
    if (!out) { std::fprintf(stderr, "MCPP_OUT_DIR unset\n"); return 1; }
    std::ofstream f(std::string(out) + "/gen_dep.cpp");
    f << "extern \"C\" int dep_gen_answer() { return 77; }\n";
    if (!f) return 1;
    std::printf("mcpp:generated=gen_dep.cpp\n");
    std::printf("mcpp:cfg=DEP_HAS_BP\n");
    return 0;
}
EOF

cat > "$TMP/deplib/src/dep.cpp" <<'EOF'
#ifndef DEP_HAS_BP
#error "dep build.mcpp define did not reach the dep's own TU"
#endif
extern "C" int dep_gen_answer();
extern "C" int dep_answer() { return dep_gen_answer(); }
EOF

cat > "$TMP/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies.deplib]
path = "../deplib"
EOF

cat > "$TMP/app/src/main.cpp" <<'EOF'
#ifdef DEP_HAS_BP
#error "dep build.mcpp define leaked into the consumer TU"
#endif
import std;
extern "C" int dep_answer();
int main() {
    std::println("dep = {}", dep_answer());
    return 0;
}
EOF

# Snapshot the dep root so we can prove it is never written to.
before="$(find "$TMP/deplib" | sort)"

cd "$TMP/app"
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "dep = 77" ]] || { echo "unexpected output: $out"; exit 1; }

# Dep root untouched (no target/, no generated files, nothing).
after="$(find "$TMP/deplib" | sort)"
[[ "$before" == "$after" ]] || {
    diff <(echo "$before") <(echo "$after") || true
    echo "dependency root was written to"; exit 1; }

# Artifacts live in the consuming project.
find target/.build-mcpp/deps -name 'gen_dep.cpp' | grep -q . || {
    echo "dep OUT_DIR generated file not in project tree"; exit 1; }

echo "OK"
