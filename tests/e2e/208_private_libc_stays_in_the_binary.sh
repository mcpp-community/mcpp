#!/usr/bin/env bash
# requires: gcc elf unix-shell
# 208_private_libc_stays_in_the_binary.sh — mcpp#401.
#
# mcpp ships a private glibc. Its libc.so.6 and its ld.so are version-locked to
# each other through GLIBC_PRIVATE symbols — glibc 2.44's libc.so.6 carries an
# undefined reference to `__pointer_chk_guard`, exported only by 2.44's own
# loader. An mcpp-built program is unaffected: PT_INTERP names the private
# loader. `/bin/sh` is not: its PT_INTERP names the HOST loader, and no
# environment variable can override it.
#
# So the moment the private libc directory reaches LD_LIBRARY_PATH — which is
# inherited by every process the program ever spawns — any popen()/system() in
# the program dies during relocation, before main:
#
#   sh: symbol lookup error: …/xim-x-glibc/2.44/lib64/libc.so.6:
#       undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
#
# The reporter hit it through an application probing the desktop theme with
# `gsettings`: the child never ran, popen() returned nothing, and the app
# silently used the wrong theme. Nothing in the mcpp output said anything.
#
# The directory is still needed — a dlopen()'d library's own DT_NEEDED closure
# does not consult the executable's RUNPATH — so it is published through the
# ARTIFACT (DT_RUNPATH, which reaches exactly the object carrying it) instead
# of the ENVIRONMENT. This test pins both halves: the child survives, and the
# library still loads.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
mkdir -p app/src app/runtime

# A dlopen-only plugin. Its presence is what makes mcpp publish the private
# libc directory at all, so the test would not exercise the bug without it.
cat > app/runtime/plugin.c <<'EOF'
int runtime_plugin_answer(void) { return 42; }
EOF
gcc -shared -fPIC app/runtime/plugin.c -o app/runtime/libruntime_plugin.so

cat > app/src/main.cpp <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

using answer_fn = int (*)();

int main() {
    // 1. What the program was handed. Printed so the test can assert on the
    //    real environment rather than on how it was constructed.
    const char* ldlp = std::getenv("LD_LIBRARY_PATH");
    std::printf("LDLP=[%s]\n", ldlp ? ldlp : "");

    // 2. A child that the HOST loader loads. This is the #401 failure.
    std::FILE* pipe = ::popen("/bin/sh -c 'echo mcpp-child-ok'", "r");
    if (!pipe) { std::puts("CHILD=popen-failed"); return 10; }
    char buf[64] = {0};
    const bool got = std::fgets(buf, sizeof buf, pipe) != nullptr;
    const int status = ::pclose(pipe);
    buf[strcspn(buf, "\n")] = '\0';
    std::printf("CHILD=[%s] status=%d\n", got ? buf : "", status);
    if (!got || std::strcmp(buf, "mcpp-child-ok") != 0) return 11;

    // 3. …and the dlopen the private libc directory exists to serve must still
    //    work, or the fix traded one breakage for another.
    void* handle = ::dlopen("libruntime_plugin.so", RTLD_NOW);
    if (!handle) { std::printf("DLOPEN=[%s]\n", ::dlerror()); return 12; }
    auto answer = reinterpret_cast<answer_fn>(
        ::dlsym(handle, "runtime_plugin_answer"));
    const int value = answer ? answer() : -1;
    ::dlclose(handle);
    std::printf("DLOPEN=ok answer=%d\n", value);
    return value == 42 ? 0 : 13;
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
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }

NINJA=$(find target -name build.ninja | head -1)
GLIBC_LIB=$(grep -oE '/[^ ",]*/xim-x-glibc/[0-9.]+/lib(64)?' "$NINJA" | head -1 || true)
if [[ -z "$GLIBC_LIB" ]]; then
    echo "SKIP: this toolchain has no private glibc payload"
    exit 0
fi
echo "private libc payload: $GLIBC_LIB"

# ── 1. published through the artifact ───────────────────────────────
# The link model emits this next to --dynamic-linker. mcpp does not add a
# second copy — the link line has a hard 128KiB ceiling — so this asserts the
# coverage it relies on instead of duplicating it. If a toolchain ever stops
# providing it, dlopen() resolution would silently lose the payload libc, and
# this is the test that says so.
grep -F -- "-Wl,-rpath,$GLIBC_LIB" "$NINJA" >/dev/null || {
    echo "FAIL: private libc directory is not in the artifact RUNPATH"
    grep -nF -- "$GLIBC_LIB" "$NINJA" || true
    exit 1
}

run_out=$("$MCPP" run 2>&1) || { echo "$run_out"; echo "run failed"; exit 1; }

# ── 2. the program's own environment must not carry it ──────────────
ldlp_line=$(printf '%s\n' "$run_out" | grep '^LDLP=' || true)
[[ -n "$ldlp_line" ]] || { echo "$run_out"; echo "FAIL: program printed no LD_LIBRARY_PATH"; exit 1; }
case "$ldlp_line" in
    *"$GLIBC_LIB"*)
        echo "$ldlp_line"
        echo "FAIL: private libc directory reached LD_LIBRARY_PATH — every child"
        echo "      process the program spawns now loads it under the host loader"
        exit 1
        ;;
esac

# ── 3. the child the host loader loads must survive ─────────────────
printf '%s\n' "$run_out" | grep -q 'CHILD=\[mcpp-child-ok\] status=0' || {
    echo "$run_out"
    echo "FAIL: a /bin/sh child did not run cleanly under mcpp run (mcpp#401)"
    exit 1
}

# ── 4. …and the dlopen it exists for still resolves ─────────────────
printf '%s\n' "$run_out" | grep -q 'DLOPEN=ok answer=42' || {
    echo "$run_out"
    echo "FAIL: dlopen through the runtime library dir regressed"
    exit 1
}

echo "OK"
