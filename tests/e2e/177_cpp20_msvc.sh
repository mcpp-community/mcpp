#!/usr/bin/env bash
# requires: msvc
# 177_cpp20_msvc.sh — standard = "c++20" on native cl.exe.
#
# This is the hard assertion behind the review decision to let MSVC answer 20
# (design §2.3/§5.3): microsoft/STL#3945 was fixed by STL#3977 (first shipping
# in VS 2022 17.8 = cl 19.38), yet Microsoft Learn still documents
# /std:c++latest as a requirement for `import std;`. Documentation cannot settle
# this — a real cl must. If this test fails on a 19.38+ runner, the premise is
# wrong and msvc::std_module_min_level goes back to 23.
#
# Asserts: /std:c++20 is what reaches the command lines (not /std:c++latest),
# `import std;` compiles and runs, and the runner's cl version is printed so the
# log records WHICH toolset the claim was verified on.
set -e

CONF="${MCPP_HOME:-$HOME/.mcpp}/config.toml"
ORIG_DEFAULT=""
if [[ -f "$CONF" ]]; then
    ORIG_DEFAULT=$(sed -n '/^\[toolchain\]/,/^\[/p' "$CONF" \
        | grep -E '^default[[:space:]]*=' | head -1 | cut -d'"' -f2 || true)
fi
TMP=$(mktemp -d)
restore() {
    if [[ -n "$ORIG_DEFAULT" ]]; then
        "$MCPP" toolchain default "$ORIG_DEFAULT" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP"
}
trap restore EXIT

cd "$TMP"
"$MCPP" toolchain default msvc >/dev/null \
    || { echo "FAIL: toolchain default msvc"; exit 1; }

# Record the toolset this run verifies (design §5.3: the version must be a fact
# in the log, not an estimate in a document).
"$MCPP" toolchain list 2>&1 | grep -i msvc || true

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
cat > mcpp.toml <<'EOF'
[package]
name     = "cpp20cl"
version  = "0.1.0"
standard = "c++20"
EOF
cat > src/main.cpp <<'EOF'
import std;
int main() {
    std::vector<int> v{1, 2, 3};
    std::cout << "cl20 " << v.size() << "\n";
    return 0;
}
EOF

build_out=$("$MCPP" build 2>&1) || {
    echo "$build_out"
    echo "FAIL: msvc c++20 + import std build failed."
    echo "      If this runner's cl is >= 19.38, the design premise (MSVC"
    echo "      answers 20) is wrong — revert msvc::std_module_min_level to 23."
    exit 1
}
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: msvc c++20 run: $run_out"; exit 1; }
[[ "$run_out" == *"cl20 3"* ]] || { echo "FAIL: run output: $run_out"; exit 1; }

# The MSVC spelling of level 20 is /std:c++20 — the `level <= 20` row in
# dialect.cppm that was unreachable until c++20 joined the allow-list.
grep -q '/std:c++20' compile_commands.json || {
    echo "FAIL: compile_commands.json missing /std:c++20"
    exit 1
}
if grep -q '/std:c++latest' compile_commands.json; then
    echo "FAIL: /std:c++latest reached the command line under standard = c++20"
    exit 1
fi

echo "PASS: msvc builds and runs standard = c++20 with import std (/std:c++20)"
