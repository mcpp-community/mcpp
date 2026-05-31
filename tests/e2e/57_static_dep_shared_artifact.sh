#!/usr/bin/env bash
# requires: elf
# A static dependency package can reference a shared dependency. Since mcpp
# flattens static package objects into the final consumer link, that final link
# must also include the shared libraries required by those static objects.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p depShared/src depStatic/src app/src

cat > depShared/src/dep.c <<'EOF'
int dep_shared_answer(void) {
    return 41;
}
EOF

cat > depShared/mcpp.toml <<'EOF'
[package]
name    = "depShared"
version = "0.1.0"

[build]
sources = ["src/*.c"]

[targets.depShared]
kind = "shared"
EOF

cat > depStatic/src/static.c <<'EOF'
extern int dep_shared_answer(void);

int dep_static_answer(void) {
    return dep_shared_answer() + 1;
}
EOF

cat > depStatic/mcpp.toml <<'EOF'
[package]
name    = "depStatic"
version = "0.1.0"

[build]
sources = ["src/*.c"]

[targets.depStatic]
kind = "lib"

[dependencies.depShared]
path = "../depShared"
EOF

cat > app/src/main.cpp <<'EOF'
extern "C" int dep_static_answer(void);

int main() {
    return dep_static_answer() == 42 ? 0 : 1;
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

[dependencies.depStatic]
path = "../depStatic"
EOF

cd app
"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "build failed"
    exit 1
}

shared_so="$(find target -name 'libdepShared.so' | head -1)"
app_bin="$(find target -path '*/bin/app' -type f | head -1)"
[[ -n "$shared_so" && -n "$app_bin" ]] || {
    cat build.log
    echo "expected shared artifact or app binary was not produced"
    exit 1
}

readelf -d "$app_bin" | grep -q 'Shared library: \[libdepShared.so\]' || {
    readelf -d "$app_bin" || true
    echo "app binary does not link against static dependency's shared dependency"
    exit 1
}

"$MCPP" run > run.log 2>&1 || {
    cat run.log
    echo "run failed"
    exit 1
}

echo "OK"
