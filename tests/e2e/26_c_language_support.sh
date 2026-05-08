#!/usr/bin/env bash
# C-language compile rule: .c files routed to `c_object` with cc / cflags,
# distinct from the .cppm/.cpp `cxx_object` rule. Verifies that a mixed
# C + modular-C++23 project links and runs, and that build.ninja contains
# the expected `c_object` / `cc` / `cflags` plumbing.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new cmix > /dev/null
cd cmix

# A pure-C source that *requires* the C frontend: uses `restrict` and
# implicit `void* → int*` from malloc(), both rejected by g++ in C++ mode.
cat > src/cmix_core.c <<'EOF'
#include <stdlib.h>
#include <string.h>
int *cmix_dup(const int *restrict src, size_t n) {
    int *out = malloc(n * sizeof(int));
    if (!out) return 0;
    memcpy(out, src, n * sizeof(int));
    return out;
}
int cmix_sum(const int *p, size_t n) {
    int s = 0;
    for (size_t i = 0; i < n; ++i) s += p[i];
    return s;
}
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" int *cmix_dup(const int *src, std::size_t n);
extern "C" int  cmix_sum(const int *p,  std::size_t n);
int main() {
    int data[] = {1, 2, 3, 4, 5};
    int *copy  = cmix_dup(data, 5);
    int  s     = cmix_sum(copy, 5);
    std::println("cmix sum = {}", s);
    std::free(copy);
    return s == 15 ? 0 : 1;
}
EOF

# Add user-supplied cflags/cxxflags so we also exercise the flag-forwarding
# path through the manifest into the per-rule baselines.
cat > mcpp.toml <<'EOF'
[package]
name        = "cmix"
version     = "0.1.0"
[build]
cflags      = ["-DCMIX_C_BUILD=1"]
cxxflags    = ["-DCMIX_CXX_BUILD=1"]
c_standard  = "c11"
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }

ninja_file="$(find target -name build.ninja | head -1)"
[[ -n "$ninja_file" ]] || { echo "no build.ninja generated"; exit 1; }

# c_object rule must be present alongside cxx_object.
grep -q '^rule c_object' "$ninja_file" || { cat "$ninja_file"; echo "missing c_object rule"; exit 1; }
# cc / cflags must be defined and pick a C compiler (gcc / cc / clang).
grep -qE '^cc        = .*(gcc|cc|clang)' "$ninja_file" || {
    echo "cc variable not pointing at a C compiler"; exit 1; }
grep -qE '^cflags    = -std=c11.*-DCMIX_C_BUILD=1' "$ninja_file" || {
    echo "cflags missing -std=c11 or user cflags tail"; exit 1; }
grep -qE '^cxxflags  = -std=c\+\+23.*-DCMIX_CXX_BUILD=1' "$ninja_file" || {
    echo "cxxflags missing -std=c++23 or user cxxflags tail"; exit 1; }
# The .c source must be routed through c_object (not cxx_object).
grep -qE 'build obj/cmix_core\.o : c_object .*cmix_core\.c' "$ninja_file" || {
    echo "cmix_core.c not routed to c_object rule"; exit 1; }
grep -qE 'build obj/cmix_core\.o : cxx_object' "$ninja_file" && {
    echo "cmix_core.c was incorrectly routed to cxx_object"; exit 1; } || true

# Run the binary; it must print "cmix sum = 15" and exit 0.
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "cmix sum = 15" ]] || {
    echo "unexpected output: $out"; exit 1; }

echo "OK"
