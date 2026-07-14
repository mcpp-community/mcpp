#!/usr/bin/env bash
# requires: gcc
# 100_cppfly_reflection.sh — standard = "c++fly" (design 2026-07-14): ONE
# manifest line gives the toolchain's latest -std= level + every experimental
# gate it supports. On gcc >= 16 that means -std=c++26 -freflection with zero
# hand-written flags, reaching every TU, the scan and the std BMI prebuild.
# Tail section: standard = "c++latest" regression — it used to reach the GNU
# driver as the invalid spelling -std=c++latest (design §11-Q5).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Capability probe (mirrors 98): newest gcc payload must accept -freflection.
USER_MCPP="${MCPP_HOME:-$HOME/.mcpp}"
GXX=$(find "$USER_MCPP/registry/data/xpkgs/xim-x-gcc" -name "g++" -path "*/bin/*" 2>/dev/null | sort | tail -1)
if [[ -z "$GXX" ]] || ! echo 'int main(){}' | "$GXX" -freflection -std=c++26 -x c++ - -fsyntax-only -o /dev/null 2>/dev/null; then
    echo "SKIP-INLINE: no -freflection-capable gcc payload available"
    exit 0
fi
GCC_VER=$(basename "$(dirname "$(dirname "$GXX")")")

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<EOF
[package]
name     = "flydemo"
version  = "0.1.0"
standard = "c++fly"

[toolchain]
default = "gcc@${GCC_VER}"
EOF

cat > src/main.cpp <<'EOF'
import std;

void print_struct(auto &&value) {
  constexpr auto info = std::meta::remove_cvref(^^decltype(value));
  constexpr auto no_check = std::meta::access_context::unchecked();
  static constexpr auto members = std::define_static_array(
      std::meta::nonstatic_data_members_of(info, no_check));
  template for (constexpr auto e : members) {
    auto &&member = value.[:e:];
    std::println("{} {}", identifier_of(e), member);
  }
}

struct Point { double x{}; double y{}; };
int main() {
  print_struct(Point{2, 3});
}
EOF

"$MCPP" build --no-cache > "$TMP/build.log" 2>&1 || {
    cat "$TMP/build.log"
    echo "FAIL: c++fly build failed"
    exit 1
}

# The c++fly summary: the value's contract is saying what was enabled/skipped.
grep -q 'c++fly on gcc' "$TMP/build.log" || {
    cat "$TMP/build.log"
    echo "FAIL: build log missing c++fly summary line"
    exit 1
}
grep -q 'reflection (-freflection)' "$TMP/build.log" || {
    cat "$TMP/build.log"
    echo "FAIL: summary missing enabled reflection gate"
    exit 1
}
grep -q 'enabled: .*contracts' "$TMP/build.log" || {
    cat "$TMP/build.log"
    echo "FAIL: summary missing enabled contracts (on by -std=c++26 on gcc16)"
    exit 1
}

# Latest level + gate flags reach the global cxxflags (every TU, deps incl.).
build_ninja="$(find target -name build.ninja | head -1)"
grep -qE '^cxxflags  = -std=c\+\+26 -freflection' "$build_ninja" || {
    grep '^cxxflags' "$build_ninja" || true
    echo "FAIL: build.ninja cxxflags lacks -std=c++26 -freflection"
    exit 1
}
if grep -q -- '-std=c++fly' "$build_ninja" compile_commands.json; then
    echo "FAIL: raw c++fly canonical leaked into a command line"
    exit 1
fi
grep -q '"-freflection"' compile_commands.json || {
    echo "FAIL: compile_commands.json missing -freflection"
    exit 1
}

# Reflection actually works at runtime (std::meta over struct members).
binary=$(find target -type f -path '*/bin/flydemo' | head -1)
out=$("$binary")
[[ "$out" == *"x 2"* && "$out" == *"y 3"* ]] || {
    echo "FAIL: reflection output: $out"
    exit 1
}

# The std BMI prebuild carries the same dialect (scan/prebuild/compile agree).
metadata="$(find "$MCPP_HOME/bmi" -name std-module.json | head -1)"
grep -q '"std_flag": "-std=c++26 -freflection' "$metadata" || {
    cat "$metadata"
    echo "FAIL: std-module.json std_flag lacks -std=c++26 -freflection"
    exit 1
}

# ── c++latest regression (design §11-Q5) ────────────────────────────────
# Used to emit the invalid GNU spelling -std=c++latest; must now resolve to
# the family latest level.
mkdir -p "$TMP/latest/src"
cd "$TMP/latest"
cat > mcpp.toml <<EOF
[package]
name     = "latestdemo"
version  = "0.1.0"
standard = "c++latest"

[toolchain]
default = "gcc@${GCC_VER}"
EOF
cat > src/main.cpp <<'EOF'
import std;
int main() { std::println("latest ok"); }
EOF

"$MCPP" build --no-cache > "$TMP/latest-build.log" 2>&1 || {
    cat "$TMP/latest-build.log"
    echo "FAIL: c++latest build failed"
    exit 1
}
latest_ninja="$(find target -name build.ninja | head -1)"
grep -qE '^cxxflags  = -std=c\+\+26' "$latest_ninja" || {
    grep '^cxxflags' "$latest_ninja" || true
    echo "FAIL: c++latest did not resolve to -std=c++26"
    exit 1
}
if grep -q -- '-std=c++latest' "$latest_ninja" compile_commands.json; then
    echo "FAIL: invalid -std=c++latest spelling reached a command line"
    exit 1
fi

echo "PASS: c++fly = latest std + experimental gates; c++latest spelling fixed"
