#!/usr/bin/env bash
# requires: elf gcc
# All module interface extensions (.cppm .ccm .cxxm .ixx) must compile and link
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new moduleext > /dev/null
cd moduleext

# ---- module interface sources (one per extension) ----
cat > src/greet.cppm <<'EOF'
export module moduleext.greet;
import std;
export auto greet(std::string_view who) -> void {
    std::println("Hello from .cppm, {}!", who);
}
EOF

cat > src/info.ccm <<'EOF'
export module moduleext.info;
import std;
export auto info(std::string_view what) -> void {
    std::println("Info from .ccm: {}", what);
}
EOF

cat > src/stat.cxxm <<'EOF'
export module moduleext.stat;
import std;
export auto stat(int n) -> void {
    std::println("Stat from .cxxm: {}", n);
}
EOF

cat > src/data.ixx <<'EOF'
export module moduleext.data;
import std;
export auto data(std::string_view key) -> void {
    std::println("Data from .ixx: {}", key);
}
EOF

# ---- consumer ----
cat > src/main.cpp <<'EOF'
import std;
import moduleext.greet;
import moduleext.info;
import moduleext.stat;
import moduleext.data;
int main() {
    greet("world");
    info("build");
    stat(42);
    data("key");
    std::println("All module extensions OK");
    return 0;
}
EOF

# Default glob only picks up .cppm/.cpp/.cc/.c — explicitly add .ccm/.cxxm/.ixx
cat > mcpp.toml <<'EOF'
[package]
name        = "moduleext"
version     = "0.1.0"
[modules]
sources = ["src/**/*.cppm", "src/**/*.ccm", "src/**/*.cxxm", "src/**/*.ixx", "src/**/*.cpp"]
EOF

# ---- build & run ----
"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "FAIL: build failed"
    exit 1
}

out="$("$MCPP" run 2>&1)"
[[ "$out" == *"from .cppm"* ]] || { echo "module greet (.cppm) not invoked: $out"; exit 1; }
[[ "$out" == *"from .ccm"*  ]] || { echo "module info  (.ccm) not invoked: $out"; exit 1; }
[[ "$out" == *"from .cxxm"* ]] || { echo "module stat  (.cxxm) not invoked: $out"; exit 1; }
[[ "$out" == *"from .ixx"*  ]] || { echo "module data  (.ixx) not invoked: $out"; exit 1; }
[[ "$out" == *"All module extensions OK"* ]] || { echo "final message missing: $out"; exit 1; }

# ---- verify build.ninja ----
build_ninja="$(find target -name build.ninja | head -1)"
[[ -n "$build_ninja" ]] || { echo "no build.ninja generated"; exit 1; }

# 4 BMIs
grep -q "gcm.cache/moduleext.greet.gcm" "$build_ninja" || { echo "ninja missing greet BMI"; exit 1; }
grep -q "gcm.cache/moduleext.info.gcm"  "$build_ninja" || { echo "ninja missing info BMI";  exit 1; }
grep -q "gcm.cache/moduleext.stat.gcm"  "$build_ninja" || { echo "ninja missing stat BMI";  exit 1; }
grep -q "gcm.cache/moduleext.data.gcm"  "$build_ninja" || { echo "ninja missing data BMI";  exit 1; }

# 4 .m.o objects (one per module interface extension)
grep -q "greet.m.o" "$build_ninja" || { echo "ninja missing greet.m.o (.cppm)"; exit 1; }
grep -q "info.m.o"  "$build_ninja" || { echo "ninja missing info.m.o  (.ccm)";  exit 1; }
grep -q "stat.m.o"  "$build_ninja" || { echo "ninja missing stat.m.o  (.cxxm)"; exit 1; }
grep -q "data.m.o"  "$build_ninja" || { echo "ninja missing data.m.o  (.ixx)";  exit 1; }

echo "OK"
