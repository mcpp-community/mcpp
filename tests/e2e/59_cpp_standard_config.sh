#!/usr/bin/env bash
# requires: gcc
# C++ standard config: [package].standard drives build flags, CDB, and std BMI.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<'EOF'
[package]
name     = "cpp26_std"
version  = "0.1.0"
standard = "c++26"
EOF

cat > src/main.cpp <<'EOF'
import std;

int main() {
    std::println("cpp standard {}", 26);
    return 0;
}
EOF

"$MCPP" build --no-cache > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: C++26 import-std build failed"
    exit 1
}

binary=$(find target -type f -path '*/bin/cpp26_std' | head -1)
[[ -n "$binary" && -x "$binary" ]] || {
    find target -maxdepth 5 -type f
    echo "FAIL: cpp26_std binary missing"
    exit 1
}

out=$("$binary")
[[ "$out" == "cpp standard 26" ]] || {
    echo "FAIL: wrong runtime output: $out"
    exit 1
}

build_ninja="$(find target -name build.ninja | head -1)"
[[ -n "$build_ninja" ]] || { echo "FAIL: build.ninja missing"; exit 1; }

grep -qE '^cxxflags  = -std=c\+\+26' "$build_ninja" || {
    echo "FAIL: build.ninja missing C++26 standard flag"
    cat "$build_ninja"
    exit 1
}
if grep -q -- "-std=c++23" "$build_ninja"; then
    echo "FAIL: build.ninja still contains C++23"
    cat "$build_ninja"
    exit 1
fi

grep -q '"-std=c++26"' compile_commands.json || {
    echo "FAIL: compile_commands.json missing C++26 standard flag"
    cat compile_commands.json
    exit 1
}
if grep -q -- "-std=c++23" compile_commands.json; then
    echo "FAIL: compile_commands.json still contains C++23"
    cat compile_commands.json
    exit 1
fi

metadata="$(find "$MCPP_HOME/bmi" -name std-module.json | head -1)"
[[ -n "$metadata" ]] || { echo "FAIL: std module metadata missing"; exit 1; }
grep -q '"cpp_standard": "c++26"' "$metadata" || {
    echo "FAIL: std module metadata missing C++26 standard"
    cat "$metadata"
    exit 1
}
grep -q '"std_flag": "-std=c++26"' "$metadata" || {
    echo "FAIL: std module metadata missing C++26 flag"
    cat "$metadata"
    exit 1
}

rm -rf target compile_commands.json
MCPP_SCANNER=p1689 "$MCPP" build --no-cache > "$TMP/build-p1689.log" 2>&1 || {
    cat "$TMP/build-p1689.log"
    echo "FAIL: C++26 P1689 scanner build failed"
    exit 1
}
p1689_ninja="$(find target -name build.ninja | head -1)"
grep -qE '^cxxflags  = -std=c\+\+26' "$p1689_ninja" || {
    echo "FAIL: P1689 build.ninja missing C++26 standard flag"
    cat "$p1689_ninja"
    exit 1
}
if grep -q -- "-std=c++23" "$TMP/build-p1689.log" "$p1689_ninja" compile_commands.json; then
    echo "FAIL: P1689 path still contains C++23"
    cat "$TMP/build-p1689.log"
    cat "$p1689_ninja"
    cat compile_commands.json
    exit 1
fi

echo "OK"
