#!/usr/bin/env bash
# requires: gcc
# Package-owned build flags: a dependency's own cflags/cxxflags must be
# applied when compiling that dependency's C/C++ source files. The consumer
# project intentionally does not declare these macros.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p depC/src app/src

cat > depC/src/dep_core.c <<'EOF'
#ifndef DEP_C_BUILD
#error "missing dependency-owned C flag"
#endif
int dep_c_answer(void) {
    return DEP_C_BUILD;
}
EOF

cat > depC/src/dep_extra.cpp <<'EOF'
#ifndef DEP_CXX_BUILD
#error "missing dependency-owned C++ flag"
#endif
extern "C" int dep_cxx_answer(void) {
    return DEP_CXX_BUILD;
}
EOF

cat > depC/mcpp.toml <<'EOF'
[package]
name    = "depC"
version = "0.1.0"

[build]
sources  = ["src/*.c", "src/*.cpp"]
cflags   = ["-DDEP_C_BUILD=7"]
cxxflags = ["-DDEP_CXX_BUILD=35"]

[targets.depC]
kind = "lib"
EOF

cat > app/src/main.cpp <<'EOF'
extern "C" int dep_c_answer(void);
extern "C" int dep_cxx_answer(void);

int main() {
    return dep_c_answer() + dep_cxx_answer() == 42 ? 0 : 1;
}
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies.depC]
path = "../depC"
EOF

cd app
"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "build failed"
    exit 1
}

"$MCPP" run > run.log 2>&1 || {
    cat run.log
    echo "run failed"
    exit 1
}

ninja_file="$(find target -name build.ninja | head -1)"
[[ -n "$ninja_file" ]] || {
    echo "no build.ninja generated"
    exit 1
}

grep -q -- "-DDEP_C_BUILD=7" "$ninja_file" || {
    echo "dep cflags missing from build.ninja"
    exit 1
}
grep -q -- "-DDEP_CXX_BUILD=35" "$ninja_file" || {
    echo "dep cxxflags missing from build.ninja"
    exit 1
}

echo "OK"
