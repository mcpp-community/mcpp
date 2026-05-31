#!/usr/bin/env bash
# requires: elf
# Dependency shared libraries must be built as package artifacts and linked by
# consumers, not flattened into the consumer binary as object files.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p depShared/src app/src

cat > depShared/src/dep.c <<'EOF'
int dep_shared_answer(void) {
    return 42;
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

cat > app/src/main.cpp <<'EOF'
extern "C" int dep_shared_answer(void);

int main() {
    return dep_shared_answer() == 42 ? 0 : 1;
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

[dependencies.depShared]
path = "../depShared"
EOF

cd app
"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "build failed"
    exit 1
}

so="$(find target -name 'libdepShared.so' | head -1)"
[[ -n "$so" ]] || {
    cat build.log
    echo "dependency shared library artifact was not produced"
    exit 1
}

app_bin="$(find target -path '*/bin/app' -type f | head -1)"
[[ -n "$app_bin" ]] || {
    cat build.log
    echo "app binary was not produced"
    exit 1
}

readelf -d "$app_bin" | grep -q 'Shared library: \[libdepShared.so\]' || {
    readelf -d "$app_bin" || true
    echo "app binary does not link against libdepShared.so"
    exit 1
}

"$MCPP" run > run.log 2>&1 || {
    cat run.log
    echo "run failed"
    exit 1
}

echo "OK"
