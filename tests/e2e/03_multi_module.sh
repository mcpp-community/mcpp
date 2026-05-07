#!/usr/bin/env bash
# Multi-module: package with .cppm + main.cpp importing it
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new multi
cd multi

# Add a module
cat > src/greeter.cppm <<'EOF'
export module multi.greeter;
import std;
export auto greet(std::string_view who) -> void {
    std::println("Hello, {}! (from multi.greeter)", who);
}
EOF

cat > src/main.cpp <<'EOF'
import std;
import multi.greeter;
int main() {
    greet("modular C++23");
    std::println("multi-module build OK");
    return 0;
}
EOF

# Build & run
"$MCPP" build > /dev/null
out=$("$MCPP" run 2>&1)
[[ "$out" == *"from multi.greeter"* ]] || { echo "module greet not invoked: $out"; exit 1; }
[[ "$out" == *"multi-module build OK"* ]] || { echo "main println missing: $out"; exit 1; }

# Verify build.ninja contents
build_ninja="$(find target -name build.ninja | head -1)"
grep -q "gcm.cache/multi.greeter.gcm"  "$build_ninja" || { echo "ninja missing greeter BMI"; exit 1; }
grep -q "gcm.cache/std.gcm"            "$build_ninja" || { echo "ninja missing std BMI"; exit 1; }

echo "OK"
