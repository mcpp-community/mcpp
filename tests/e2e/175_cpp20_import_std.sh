#!/usr/bin/env bash
# requires: gcc
# 175_cpp20_import_std.sh — standard = "c++20" is a first-class level, and
# `import std;` still works there.
#
# `import std;` is a C++23 *library* feature, but named modules are C++20 and
# every implementation mcpp ships provides the std module in C++20 mode too
# (libstdc++'s bits/std.cc and libc++'s std.cppm carry no __cplusplus guard;
# MSVC STL unblocked it in microsoft/STL#3977). Verified by on-machine probes
# across gcc 15/16 x glibc/musl/mingw and clang 22 + libc++ — see
# .agents/docs/2026-07-31-cpp20-standard-support-design.md §2.2.
#
# Asserts: build + run succeed, -std=c++20 reaches every surface (global
# cxxflags, compile_commands.json, the std BMI prebuild), and no c++23 leaks in.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<'EOF'
[package]
name     = "cpp20demo"
version  = "0.1.0"
standard = "c++20"
EOF

# Deliberately C++20-only library usage: no std::print / std::expected here.
cat > src/util.cppm <<'EOF'
export module cpp20demo.util;

import std;

export namespace cpp20demo {
inline int count_evens(const std::vector<int>& v) {
    return static_cast<int>(std::ranges::count_if(v, [](int x) { return x % 2 == 0; }));
}
}
EOF

cat > src/main.cpp <<'EOF'
import std;
import cpp20demo.util;

int main() {
    std::vector<int> v{1, 2, 3, 4, 5, 6};
    std::cout << "evens=" << cpp20demo::count_evens(v) << "\n";
    static_assert(__cplusplus == 202002L, "expected C++20 mode");
    return 0;
}
EOF

"$MCPP" build --no-cache > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: c++20 build failed"
    exit 1
}

binary=$(find target -type f -path '*/bin/cpp20demo' | head -1)
out=$("$binary")
[[ "$out" == "evens=3" ]] || {
    echo "FAIL: unexpected output: $out"
    exit 1
}

# The level reaches the module-graph-global cxxflags (every TU, deps included).
build_ninja="$(find target -name build.ninja | head -1)"
grep -qE '^cxxflags  = -std=c\+\+20' "$build_ninja" || {
    grep '^cxxflags' "$build_ninja" || true
    echo "FAIL: build.ninja cxxflags is not -std=c++20"
    exit 1
}
if grep -q -- '-std=c++23' "$build_ninja" compile_commands.json; then
    echo "FAIL: -std=c++23 leaked into a command line under standard = c++20"
    exit 1
fi
grep -q -- '-std=c++20' compile_commands.json || {
    echo "FAIL: compile_commands.json missing -std=c++20"
    exit 1
}

# The std BMI prebuild must use the SAME level as the TUs importing it — a
# c++23 std.gcm fed to a c++20 TU is rejected outright ("language dialect
# differs"), so this is the cache-identity assertion, not cosmetics.
grep -rl '"std_flag": "-std=c++20"' "$MCPP_HOME/build-cache/v1/std" >/dev/null 2>&1 || {
    find "$MCPP_HOME/build-cache/v1/std" -name std-module.json \
        -exec grep -H '"std_flag"' {} \; 2>/dev/null
    echo "FAIL: no std-module.json records std_flag -std=c++20"
    exit 1
}

# gnu++20 is accepted too and keeps the GNU dialect spelling.
mkdir -p "$TMP/gnu/src"
cd "$TMP/gnu"
cat > mcpp.toml <<'EOF'
[package]
name     = "gnu20demo"
version  = "0.1.0"
standard = "gnu++20"
EOF
cat > src/main.cpp <<'EOF'
import std;
int main() { std::cout << "gnu20 ok\n"; }
EOF
"$MCPP" build --no-cache > "$TMP/gnu-build.log" 2>&1 || {
    cat "$TMP/gnu-build.log"
    echo "FAIL: gnu++20 build failed"
    exit 1
}
gnu_ninja="$(find target -name build.ninja | head -1)"
grep -qE '^cxxflags  = -std=gnu\+\+20' "$gnu_ninja" || {
    grep '^cxxflags' "$gnu_ninja" || true
    echo "FAIL: gnu++20 did not reach cxxflags"
    exit 1
}

# Below the floor stays rejected, and the message names the floor.
mkdir -p "$TMP/old/src"
cd "$TMP/old"
cat > mcpp.toml <<'EOF'
[package]
name     = "olddemo"
version  = "0.1.0"
standard = "c++17"
EOF
cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF
if "$MCPP" build --no-cache > "$TMP/old-build.log" 2>&1; then
    echo "FAIL: standard = c++17 was accepted"
    exit 1
fi
grep -q 'c++20' "$TMP/old-build.log" || {
    cat "$TMP/old-build.log"
    echo "FAIL: rejection message does not advertise the c++20 floor"
    exit 1
}

echo "PASS: c++20 is a first-class level, import std works, gnu++20 ok, c++17 rejected"
