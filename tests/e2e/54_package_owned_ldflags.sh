#!/usr/bin/env bash
# requires: gcc
# Package-owned ldflags: a dependency can declare link flags that are applied
# when the consumer binary is linked.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p native depLink app/src

cat > native/answer.c <<'EOF'
int dep_link_answer(void) {
    return 42;
}
EOF

gcc -c native/answer.c -o native/answer.o
ar rcs native/libanswer.a native/answer.o
NATIVE_DIR="$(pwd)/native"

cat > depLink/mcpp.toml <<EOF
[package]
name    = "depLink"
version = "0.1.0"

[build]
ldflags = ["-L${NATIVE_DIR}", "-lanswer"]
EOF

cat > app/src/main.cpp <<'EOF'
extern "C" int dep_link_answer(void);

int main() {
    return dep_link_answer() == 42 ? 0 : 1;
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

[dependencies.depLink]
path = "../depLink"
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

grep -q -- "-L${NATIVE_DIR}" "$ninja_file" || {
    echo "dep -L flag missing from build.ninja"
    cat "$ninja_file"
    exit 1
}
grep -q -- "-lanswer" "$ninja_file" || {
    echo "dep -l flag missing from build.ninja"
    cat "$ninja_file"
    exit 1
}

echo "OK"
