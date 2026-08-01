#!/usr/bin/env bash
# requires: fresh-sandbox
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
triple=$(ls -d target/*/ | head -1)
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

# 3. Branch refs should update when the user asks for mcpp update.
mkdir "$TMP/branch-origin" && cd "$TMP/branch-origin"
git init --quiet
git checkout -B main --quiet
git config user.email "test@local"
git config user.name  "test"
"$MCPP" new branchlib >/dev/null
mv branchlib/* branchlib/.??* . 2>/dev/null || true
rmdir branchlib 2>/dev/null || true
cat > src/greet.cppm <<'EOF'
export module branchlib.greet;
import std;
export auto greet() -> void { std::println("branch dep v1"); }
EOF
rm src/main.cpp
cat > mcpp.toml <<'EOF'
[package]
name        = "branchlib"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm"]
[targets.branchlib]
kind = "lib"
EOF
git add -A >/dev/null
git commit --quiet -m "v1"

cd "$TMP"
"$MCPP" new branchapp >/dev/null
cd branchapp
cat > src/main.cpp <<'EOF'
import std;
import branchlib.greet;
int main() { greet(); return 0; }
EOF
cat > mcpp.toml <<EOF
[package]
name        = "branchapp"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]
[targets.branchapp]
kind = "bin"
main = "src/main.cpp"
[dependencies.branchlib]
git = "$TMP/branch-origin"
branch = "main"
EOF

"$MCPP" build > branch-v1.log 2>&1
triple=$(ls -d target/*/ | head -1)
fp_dir=$(ls "$triple")
out=$(${triple}${fp_dir}/bin/branchapp)
[[ "$out" == *"branch dep v1"* ]] || {
    echo "FAIL: branch dep v1 not invoked: $out"
    cat branch-v1.log; exit 1; }

# A rebuild with the lock in place must resolve the branch dep from the
# recorded commit rather than calling `git ls-remote`.
#
# `mcpp clean` first: a bare `mcpp build` here would take the fast path in
# cmd_build (build.ninja is newer than every input → just run ninja), so
# prepare_build never runs and neither the anchor nor `ls-remote` would be
# exercised — the assertion would pass identically without the feature. clean
# wipes only `target/`, so mcpp.lock and the git cache both survive.
#
# Run under MCPP_OFFLINE as well: with the commit in the lock and the clone on
# disk there is nothing left to fetch, so this must hold with the network
# refused. It also pins the local-remote rule — this fixture's `git =` is a
# directory path, which costs no network and must never be refused offline.
"$MCPP" clean >/dev/null
build2=$(MCPP_OFFLINE=1 "$MCPP" build 2>&1) || {
    echo "FAIL: offline rebuild with lock + cache present failed"
    echo "--- build2 ---"; cat <<<"$build2"; exit 1; }
echo "$build2" | grep -q 'from mcpp.lock' || { echo "FAIL: branch dep not resolved from mcpp.lock on rebuild"; echo "--- build2 ---"; cat <<<"$build2"; exit 1; }
echo "$build2" | grep -q 'Cloning' && { echo "FAIL: branch dep re-cloned on rebuild"; exit 1; } || true

grep -q 'source  = "git+' mcpp.lock || {
    echo "FAIL: git dep lock source is not marked as git"
    cat mcpp.lock; exit 1; }
grep -q 'branch=main' mcpp.lock || {
    echo "FAIL: git dep lock source does not record branch ref"
    cat mcpp.lock; exit 1; }
! grep -q 'source  = "index+' mcpp.lock || {
    echo "FAIL: git dep lock source incorrectly uses index source"
    cat mcpp.lock; exit 1; }

cd "$TMP/branch-origin"
cat > src/greet.cppm <<'EOF'
export module branchlib.greet;
import std;
export auto greet() -> void { std::println("branch dep v2"); }
EOF
git add -A >/dev/null
git commit --quiet -m "v2"

cd "$TMP/branchapp"

# mcpp.lock is authoritative, not a hint the cache has to confirm. The branch
# now points at v2 and the git cache is gone, so the only surviving record of
# what this project builds is the commit in mcpp.lock — and the build must
# still produce v1 from it. If the lock were merely an optimisation over the
# cache, evicting ~/.mcpp/git would silently advance the build to v2.
rm -rf "$MCPP_HOME/git"
"$MCPP" clean >/dev/null
"$MCPP" build > branch-evicted.log 2>&1 || {
    echo "FAIL: rebuild after git-cache eviction"
    cat branch-evicted.log; exit 1; }
triple=$(ls -d target/*/ | head -1)
fp_dir=$(ls "$triple")
out=$(${triple}${fp_dir}/bin/branchapp)
[[ "$out" == *"branch dep v1"* ]] || {
    echo "FAIL: git-cache eviction silently advanced the locked branch: $out"
    echo "--- branch-evicted.log ---"; cat branch-evicted.log
    echo "--- mcpp.lock ---"; cat mcpp.lock; exit 1; }

"$MCPP" update branchlib >/dev/null
"$MCPP" clean >/dev/null
"$MCPP" build > branch-v2.log 2>&1
triple=$(ls -d target/*/ | head -1)
fp_dir=$(ls "$triple")
out=$(${triple}${fp_dir}/bin/branchapp)
[[ "$out" == *"branch dep v2"* ]] || {
    echo "FAIL: branch dep did not refresh after mcpp update: $out"
    echo "--- branch-v2.log ---"
    cat branch-v2.log
    echo "--- mcpp.lock ---"
    cat mcpp.lock
    exit 1
}

# Cleanup so trap doesn't blow up if any subprocess holds files.
echo "OK"
