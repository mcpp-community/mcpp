#!/usr/bin/env bash
# requires: elf gcc
# `mcpp::cxx_stdlib()` -- the C++ standard library the engine resolved, handed
# to the build program.
#
# WHY IT IS NOT `compiler()`. clang links libc++ on one machine and libstdc++
# on another and answers "clang" in both cases, and the two implementations
# differ in what they accept: llama.cpp-m's Vulkan backend destroys a
# `unique_ptr` to an incomplete type in an upstream header, which libstdc++
# accepts and libc++ rejects. A package that wants to refuse that configuration
# by name, before the compiler produces a page of errors, has to be able to ask.
#
# THE CRITERION IS NOT "THE VARIABLE IS SET". An answer that arrives empty, or
# arrives as some other field's value, would also be "set". The build program
# writes what it read to a FILE -- build.mcpp's stdout is printed only when it
# fails, so a criterion that grepped the build log would be silent on success --
# and the test compares that file against two independent objects:
#
#   1. `resolution.json`, which the engine writes from its own resolved
#      toolchain. Same value, different producer and different code path.
#   2. The compiler family. GCC's C++ standard library is libstdc++, and
#      nothing else; a wiring that handed over the C library, the ABI tag or
#      the empty string would fail here even if (1) somehow agreed.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > app/build.mcpp <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    const char* stdlib   = mcpp::cxx_stdlib();
    const char* compiler = mcpp::compiler();
    // The answer goes to a file. stdout from a build program reaches the user
    // only when the program fails, so a test that asserted on the build log
    // would pass for a build that printed nothing at all.
    std::string out = std::string(mcpp::manifest_dir()) + "/answer.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 1;
    std::fprintf(f, "compiler=%s\nstdlib=%s\n",
                 compiler == nullptr ? "" : compiler,
                 stdlib   == nullptr ? "" : stdlib);
    std::fclose(f);
    return 0;
}
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "stdlibq"
version = "0.1.0"
[targets.stdlibq]
kind = "bin"
main = "src/main.cpp"
EOF

cd app
"$MCPP" build >build.log 2>&1 || { echo "FAIL: build"; cat build.log; exit 1; }

[ -f answer.txt ] || { echo "FAIL: the build program wrote no answer"; cat build.log; exit 1; }
compiler=$(sed -n 's/^compiler=//p' answer.txt)
stdlib=$(sed -n 's/^stdlib=//p' answer.txt)
echo "build program read: compiler='$compiler' stdlib='$stdlib'"

[ -n "$stdlib" ] || { echo "FAIL: mcpp::cxx_stdlib() is empty"; exit 1; }

# 1. against resolution.json, which the engine writes from its own resolution.
res=$(find target -name resolution.json | head -1)
[ -n "$res" ] || { echo "FAIL: no resolution.json under target/"; exit 1; }
engine=$(sed -n 's/.*"stdlib"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$res" | head -1)
echo "resolution.json ($res) says: '$engine'"
[ "$stdlib" = "$engine" ] || {
    echo "FAIL: build program read '$stdlib', the engine resolved '$engine'"
    exit 1
}

# 2. against the compiler family. This test runs under gcc (see `requires`),
#    whose C++ standard library is libstdc++ and is not configurable.
[ "$compiler" = "gcc" ] || { echo "FAIL: expected the gcc toolchain, got '$compiler'"; exit 1; }
[ "$stdlib" = "libstdc++" ] || {
    echo "FAIL: gcc's C++ standard library is libstdc++, the build program read '$stdlib'"
    exit 1
}

echo "PASS: mcpp::cxx_stdlib() answers '$stdlib', matching resolution.json and the compiler family"
