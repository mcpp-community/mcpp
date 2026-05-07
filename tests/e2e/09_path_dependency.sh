#!/usr/bin/env bash
# Path-based dependency: package B imports modules from package A via
#   [dependencies.A] path = "../A"
# Verifies the multi-package scanner + linker pipeline.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"

# Package A — provides module mylibA.greet
"$MCPP" new mylibA > /dev/null
cd mylibA
cat > src/greet.cppm <<'EOF'
export module mylibA.greet;
import std;
export auto greet(std::string_view who) -> void {
    std::println("Hello {} from mylibA!", who);
}
EOF
rm src/main.cpp

cat > mcpp.toml <<'EOF'
[package]
name        = "mylibA"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm"]
[targets.mylibA]
kind = "lib"
EOF
cd ..

# Package B — depends on A via path
"$MCPP" new myappB > /dev/null
cd myappB

cat > src/main.cpp <<'EOF'
import std;
import mylibA.greet;
int main() {
    greet("modular C++23");
    std::println("path dep build OK");
    return 0;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name        = "myappB"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]
[targets.myappB]
kind = "bin"
main = "src/main.cpp"
[dependencies.mylibA]
path = "../mylibA"
EOF

"$MCPP" build > build.log 2>&1
out=$("$MCPP" run 2>&1)
[[ "$out" == *"Hello modular C++23 from mylibA"* ]] || { echo "dep module not invoked: $out"; exit 1; }
[[ "$out" == *"path dep build OK"*               ]] || { echo "main println missing: $out"; exit 1; }

# build.ninja should compile sources from BOTH packages
ninja_file="$(find target -name build.ninja)"
grep -q 'mylibA.*greet.cppm' "$ninja_file" || { echo "ninja missing dep package source"; exit 1; }
grep -q 'mcpp.cache/mylibA.greet.gcm\|gcm.cache/mylibA.greet.gcm' "$ninja_file" || {
    echo "ninja missing dep BMI"; exit 1; }

# Path-resolution error reporting: declared name mismatch
TMP2=$(mktemp -d)
cp -r "$TMP/mylibA" "$TMP2/wrongname"
sed -i 's/name        = "mylibA"/name        = "differentname"/' "$TMP2/wrongname/mcpp.toml"
cat > mcpp.toml <<EOF
[package]
name        = "myappB"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]
[targets.myappB]
kind = "bin"
main = "src/main.cpp"
[dependencies.mylibA]
path = "$TMP2/wrongname"
EOF

err=$("$MCPP" build 2>&1) && { echo "expected name-mismatch error"; exit 1; }
[[ "$err" == *"mismatch with declared name"* ]] || { echo "wrong error: $err"; exit 1; }

rm -rf "$TMP2"
echo "OK"
