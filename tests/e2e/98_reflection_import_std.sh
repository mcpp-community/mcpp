#!/usr/bin/env bash
# requires: gcc
# 98_reflection_import_std.sh — issue #210: dialect-class flags reach the std
# module BMI prebuild AND every TU in the graph:
#   - [build] cxxflags = ["-freflection"] → `import std;` exposes std::meta
#   - a dependency module that imports std builds in the same graph (the
#     fmt.gcm-class secondary failure from the issue)
#   - std-module.json records the flag in the std build command
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# The resolved toolchain must accept -freflection (GCC 16+). Probe via the
# project's own toolchain resolution: build a trivial reflection TU.
GXX=$(find "${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs/xim-x-gcc" -name "g++" -path "*/bin/*" 2>/dev/null | sort | tail -1)
if [[ -z "$GXX" ]] || ! echo 'int main(){}' | "$GXX" -freflection -std=c++26 -x c++ - -fsyntax-only -o /dev/null 2>/dev/null; then
    echo "SKIP-INLINE: no -freflection-capable gcc payload available"
    exit 0
fi

mkdir -p "$TMP/proj/src" "$TMP/proj/meta_dep/src"
cd "$TMP/proj"

# Path dependency providing a module that ITSELF imports std — must be
# compiled with the same dialect set or GCC rejects the BMI mix.
cat > meta_dep/mcpp.toml <<'EOF'
[package]
name     = "meta_dep"
version  = "0.1.0"
standard = "c++26"

[targets.meta_dep]
kind = "lib"
EOF
cat > meta_dep/src/meta_dep.cppm <<'EOF'
export module meta_dep;
import std;
export namespace meta_dep {
std::string tag() { return "dep-ok"; }
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name     = "refl"
version  = "0.1.0"
standard = "c++26"

[build]
cxxflags = ["-freflection"]

[dependencies]
meta_dep = { path = "meta_dep" }
EOF
cat > src/main.cpp <<'EOF'
import std;
import meta_dep;

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
  std::println("{}", meta_dep::tag());
}
EOF

out=$("$MCPP" run 2>&1) || { echo "FAIL: build/run: $out"; exit 1; }
[[ "$out" == *"x 2"* && "$out" == *"y 3"* ]] || { echo "FAIL: reflection output: $out"; exit 1; }
[[ "$out" == *"dep-ok"* ]] || { echo "FAIL: dep module output: $out"; exit 1; }

# The std BMI build command must carry the dialect flag.
grep -rl '"std_flag": "[^"]*-freflection' "${MCPP_HOME:-$HOME/.mcpp}/bmi/" >/dev/null \
    || { echo "FAIL: std-module.json lacks -freflection in std_flag"; exit 1; }

echo "PASS: dialect flags reach std BMI + whole module graph (issue #210)"
