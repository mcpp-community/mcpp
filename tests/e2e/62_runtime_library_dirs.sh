#!/usr/bin/env bash
# requires: gcc unix-shell
# Runtime library directories declared in mcpp.toml must be visible to
# libraries loaded only through dlopen(), not just DT_NEEDED link deps.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p app/src app/tests app/runtime

cat > app/runtime/plugin.c <<'EOF'
int runtime_plugin_answer(void) {
    return 42;
}
EOF

gcc -shared -fPIC app/runtime/plugin.c -o app/runtime/libruntime_plugin.so

cat > app/src/main.cpp <<'EOF'
#include <dlfcn.h>

using answer_fn = int (*)();

int main() {
    void* handle = dlopen("libruntime_plugin.so", RTLD_NOW);
    if (!handle) {
        return 10;
    }
    auto answer = reinterpret_cast<answer_fn>(dlsym(handle, "runtime_plugin_answer"));
    if (!answer) {
        dlclose(handle);
        return 11;
    }
    int result = answer();
    dlclose(handle);
    return result == 42 ? 0 : 12;
}
EOF

cat > app/tests/test_runtime_plugin.cpp <<'EOF'
#include <dlfcn.h>

using answer_fn = int (*)();

int main() {
    void* handle = dlopen("libruntime_plugin.so", RTLD_NOW);
    if (!handle) {
        return 20;
    }
    auto answer = reinterpret_cast<answer_fn>(dlsym(handle, "runtime_plugin_answer"));
    if (!answer) {
        dlclose(handle);
        return 21;
    }
    int result = answer();
    dlclose(handle);
    return result == 42 ? 0 : 22;
}
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]
ldflags = ["-ldl"]

[runtime]
library_dirs = ["runtime"]

[targets.app]
kind = "bin"
main = "src/main.cpp"
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

"$MCPP" test > test.log 2>&1 || {
    cat test.log
    echo "test failed"
    exit 1
}

echo "OK"
