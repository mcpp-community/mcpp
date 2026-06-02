#!/usr/bin/env bash
# requires: elf
# Shared libraries can declare an ABI SONAME. Consumers may load that ABI name
# through dlopen(), and mcpp run must provide the runtime alias automatically.
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
soname = "libdepShared.so.1"
EOF

cat > app/src/main.cpp <<'EOF'
#include <dlfcn.h>

using answer_fn = int (*)();

int main() {
    void* handle = dlopen("libdepShared.so.1", RTLD_NOW);
    if (!handle) {
        return 10;
    }
    auto answer = reinterpret_cast<answer_fn>(dlsym(handle, "dep_shared_answer"));
    if (!answer) {
        dlclose(handle);
        return 11;
    }
    int result = answer();
    dlclose(handle);
    return result == 42 ? 0 : 12;
}
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]
ldflags = ["-ldl"]

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
alias="$(find target -name 'libdepShared.so.1' | head -1)"
[[ -n "$so" && -n "$alias" ]] || {
    cat build.log
    find target -path '*/bin/*' -maxdepth 4 -type f -o -type l 2>/dev/null || true
    echo "expected shared library and ABI soname alias were not produced"
    exit 1
}

readelf -d "$so" | grep -q 'Library soname: \[libdepShared.so.1\]' || {
    readelf -d "$so" || true
    echo "shared library missing requested SONAME"
    exit 1
}

"$MCPP" run > run.log 2>&1 || {
    cat run.log
    echo "run failed"
    exit 1
}

echo "OK"
