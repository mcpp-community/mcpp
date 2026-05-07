#!/usr/bin/env bash
# 24_git_dependency.sh — M4 #5: git-based dep clones to ~/.mcpp/git/<hash>/
# and is treated as a path dep.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
# Inherit toolchain from user's installed mcpp (avoids reinstalling gcc).
source "$(dirname "$0")/_inherit_toolchain.sh"
# Now override HOME — but inheritance must happen first while $HOME points
# at the user's real home.
export HOME="$TMP/home"   # so default_cache_root + git cache resolve here
mkdir -p "$HOME"

# 1. Build a tiny mcpp lib in a real local git repo (acts as origin).
mkdir "$TMP/origin" && cd "$TMP/origin"
git init --quiet
git config user.email "test@local"
git config user.name  "test"
"$MCPP" new mylibA >/dev/null
mv mylibA/* mylibA/.??* . 2>/dev/null || true
rmdir mylibA 2>/dev/null || true
cat > src/greet.cppm <<'EOF'
export module mylibA.greet;
import std;
export auto greet() -> void { std::println("hello from git dep"); }
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
git add -A >/dev/null
git commit --quiet -m "init"
HEAD_REV=$(git rev-parse HEAD)

# 2. Consumer references the git repo + rev.
cd "$TMP"
"$MCPP" new myapp >/dev/null
cd myapp
cat > src/main.cpp <<'EOF'
import std;
import mylibA.greet;
int main() { greet(); return 0; }
EOF
cat > mcpp.toml <<EOF
[package]
name        = "myapp"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]
[targets.myapp]
kind = "bin"
main = "src/main.cpp"
[dependencies.mylibA]
git = "$TMP/origin"
rev = "$HEAD_REV"
EOF

"$MCPP" build > build.log 2>&1
triple=$(ls -d target/x86_64-linux-*/ | head -1)
fp_dir=$(ls "$triple")
out=$(${triple}${fp_dir}/bin/myapp)
[[ "$out" == *"hello from git dep"* ]] || {
    echo "FAIL: git dep module not invoked: $out"
    cat build.log; exit 1; }

# Verify we cached the clone under MCPP_HOME/git
[[ -d "$MCPP_HOME/git" ]] || { echo "FAIL: \$MCPP_HOME/git not created"; ls "$MCPP_HOME"; exit 1; }
test -n "$(ls -A "$MCPP_HOME/git" 2>/dev/null)" || { echo "FAIL: git cache empty"; exit 1; }

# Reuse the same dep — second build should NOT re-clone.
build2=$("$MCPP" build 2>&1)
echo "$build2" | grep -q 'Cloning' && { echo "FAIL: re-cloned on rebuild"; exit 1; } || true

# Cleanup so trap doesn't blow up if any subprocess holds files.
echo "OK"
