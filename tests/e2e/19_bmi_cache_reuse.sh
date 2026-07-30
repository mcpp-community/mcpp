#!/usr/bin/env bash
# requires:
# 19_bmi_cache_reuse.sh — local-source packages must never enter the global
# build cache, DIRECTLY or TRANSITIVELY.
#
#   1. A direct path dep is not cached.
#   2. A path dep reached THROUGH another path dep is not cached either.
#
# (2) is the regression this file exists for. The old predicate looked the
# package up in the ROOT manifest's dependencies/dev-dependencies and skipped it
# when the spec was path/git — but a transitively-reached package is in neither
# map, so it fell through and WAS cached, with its index name defaulting to the
# default index (a workspace member `B` landed on disk as `mcpplibs/B@0.1.0`).
# Its sources can then change without changing name@version, so the cache key
# cannot see the change: a stale object, served silently.
#
# The cross-project positive case needs a registry dep and lives in
# 172_build_cache_cross_project.sh.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

# --- Part 1: build a path-dep project and verify NO cache entry is written ---
cd "$TMP"
"$MCPP" new mylibA > /dev/null
cd mylibA
cat > src/greet.cppm <<'EOF'
export module mylibA.greet;
import std;
export auto greet() -> void { std::println("hi"); }
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

"$MCPP" new myapp > /dev/null
cd myapp
cat > src/main.cpp <<'EOF'
import std;
import mylibA.greet;
int main() { greet(); return 0; }
EOF
cat > mcpp.toml <<'EOF'
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
path = "../mylibA"
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }

# The cache root exists (env init creates it) but holds no package entry for a
# path dep.
[[ -d "$MCPP_HOME/build-cache/v1" ]] || { echo "missing $MCPP_HOME/build-cache/v1"; exit 1; }
if find "$MCPP_HOME/build-cache/v1/pkg" -path "*mylibA*" 2>/dev/null | grep -q .; then
    echo "FAIL: path dep mylibA was populated into the build cache (must be skipped)"
    find "$MCPP_HOME/build-cache/v1" -maxdepth 5
    exit 1
fi

# Build output must NOT show "Cached mylibA" (it's a path dep, not a registry dep).
if grep -q 'Cached mylibA' build.log; then
    echo "FAIL: path dep wrongly labeled Cached"
    cat build.log; exit 1
fi

# --- Part 2: a path dep reached THROUGH a path dep is also excluded ---------
cd "$TMP"
"$MCPP" new mylibB > /dev/null
cd mylibB
cat > src/inner.cppm <<'EOF'
export module mylibB.inner;
export auto inner() -> int { return 7; }
EOF
rm src/main.cpp
cat > mcpp.toml <<'EOF'
[package]
name        = "mylibB"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[modules]
sources = ["src/**/*.cppm"]
[targets.mylibB]
kind = "lib"
EOF

# mylibA now depends on mylibB by path, so myapp reaches mylibB transitively
# and mylibB appears in NEITHER of myapp's dependency maps.
cd "$TMP/mylibA"
cat > src/greet.cppm <<'EOF'
export module mylibA.greet;
import std;
import mylibB.inner;
export auto greet() -> void { std::println("hi {}", inner()); }
EOF
cat >> mcpp.toml <<'EOF'
[dependencies.mylibB]
path = "../mylibB"
EOF

cd "$TMP/myapp"
rm -rf target
"$MCPP" build > build2.log 2>&1 || { cat build2.log; exit 1; }

for name in mylibA mylibB; do
    if find "$MCPP_HOME/build-cache/v1/pkg" -path "*$name*" 2>/dev/null | grep -q .; then
        echo "FAIL: local package $name entered the build cache"
        find "$MCPP_HOME/build-cache/v1/pkg" -maxdepth 4 2>/dev/null
        cat build2.log
        exit 1
    fi
done

# And `mcpp cache list` — the user-visible surface — must not list them either.
"$MCPP" cache list > list.log 2>&1
for name in mylibA mylibB; do
    if grep -q "$name" list.log; then
        echo "FAIL: cache list shows local package $name"
        cat list.log
        exit 1
    fi
done

# Editing the transitive path dep must still take effect (it is compiled, not
# served from a cache that cannot see the change).
cd "$TMP/mylibB"
cat > src/inner.cppm <<'EOF'
export module mylibB.inner;
export auto inner() -> int { return 99; }
EOF
cd "$TMP/myapp"
"$MCPP" run > run.log 2>&1 || { cat run.log; exit 1; }
grep -q 'hi 99' run.log || {
    echo "FAIL: edit to a transitive path dep did not reach the binary"
    cat run.log
    exit 1
}

echo "OK"
