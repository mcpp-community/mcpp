#!/usr/bin/env bash
# requires: gcc
# P1 (large-source-pkg design §3.1): `mcpp:include-dir=` adds a private include
# directory for the package's own TUs — replacing the old double-emission hack
# (`mcpp:cxxflag=-I…` + `mcpp:cflag=-I…`, unnormalized). Scenario: build.mcpp
# writes a header under MCPP_OUT_DIR and points include-dir at it (absolute);
# a source #includes it; the build succeeds with NO -I flag emitted manually.
# Also: `mcpp:include-dir-after=` rides the typed #249 channel end-to-end —
# the emitted build.ninja must carry -idirafter for the directive dir — and
# both directives round-trip the directive cache.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p incdir/src
cd incdir

cat > build.mcpp <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <filesystem>
import mcpp;
int main() {
    std::string inc = std::string(mcpp::out_dir()) + "/inc";
    std::filesystem::create_directories(inc);
    std::ofstream(inc + "/bp_config.h") << "#define BP_ANSWER 42\n";
    mcpp::include_dir(inc.c_str());          // absolute
    mcpp::include_dir_after("vendor/sysinc"); // relative -> package root; accepted
    return 0;
}
EOF
mkdir -p vendor/sysinc

cat > src/main.cpp <<'EOF'
import std;
#include <bp_config.h>
int main() {
    std::println("ANSWER={}", BP_ANSWER);
    return 0;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "incdir"
version = "0.1.0"

[modules]
sources = ["src/**/*.cpp"]

[targets.incdir]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" build > build1.log 2>&1 || { cat build1.log; echo "FAIL: build failed"; exit 1; }
grep -q "ignoring unknown directive" build1.log && {
    cat build1.log; echo "FAIL: include-dir directive not recognized"; exit 1; } || true

out="$("$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)"
[[ "$out" == "ANSWER=42" ]] || { echo "FAIL: include dir not on the -I chain: $out"; exit 1; }

# include-dir-after reaches the compile edges as -idirafter (typed #249
# channel: privateBuild.includeDirsAfter → localIncludeDirsAfter).
ninja_file=$(find target -name build.ninja | head -1)
grep -q -- "-idirafter.*vendor/sysinc" "$ninja_file" || {
    echo "FAIL: include-dir-after not emitted as -idirafter in build.ninja";
    grep -n "local_includes" "$ninja_file" | head; exit 1; }

# Cache round-trip: touch a source (defeats the whole-build fast path, keeps
# build.mcpp inputs unchanged) → the cached include-dir record must reapply
# without a re-run, and the header must still resolve.
cat > src/main.cpp <<'EOF'
import std;
#include <bp_config.h>
int main() {
    std::println("ANSWER2={}", BP_ANSWER);
    return 0;
}
EOF
"$MCPP" build > build2.log 2>&1 || { cat build2.log; echo "FAIL: rebuild failed"; exit 1; }
grep -q "build.mcpp running" build2.log && {
    cat build2.log; echo "FAIL: unchanged build.mcpp re-ran"; exit 1; } || true
out="$("$MCPP" run 2>&1 | grep '^ANSWER2=' | tail -1)"
[[ "$out" == "ANSWER2=42" ]] || { echo "FAIL: cached include-dir record lost: $out"; exit 1; }

echo "OK"
