#!/usr/bin/env bash
# requires: gcc
# 176_cpp20_std_bmi_isolation.sh — switching [package].standard between c++20
# and c++23 must give each level its own std BMI and its own target dir.
#
# This is not a hygiene preference: a std BMI built at one level is REJECTED by
# the compiler at another ("std: error: language dialect differs 'C++20',
# expected 'C++23'"). The standard is part of the fingerprint, of the std BMI
# identity and of the dependency build-cache key precisely so a level switch
# cannot produce a corrupt hit — this test locks that in both directions,
# including going back (the second c++23 build must still work).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > src/main.cpp <<'EOF'
import std;
int main() { std::cout << "level ok\n"; return 0; }
EOF

write_manifest() {
    cat > mcpp.toml <<EOF
[package]
name     = "leveldemo"
version  = "0.1.0"
standard = "$1"
EOF
}

build_at() {
    write_manifest "$1"
    "$MCPP" build > "$TMP/build-$1.log" 2>&1 || {
        cat "$TMP/build-$1.log"
        echo "FAIL: build at $1 failed"
        exit 1
    }
    binary=$(find target -type f -path '*/bin/leveldemo' -newer mcpp.toml | head -1)
    [[ -n "$binary" ]] || binary=$(find target -type f -path '*/bin/leveldemo' | head -1)
    out=$("$binary")
    [[ "$out" == "level ok" ]] || {
        echo "FAIL: bad output at $1: $out"
        exit 1
    }
}

build_at "c++23"
build_at "c++20"
# Back again: the c++23 artifacts must still be valid and reusable, not
# clobbered by the c++20 run.
build_at "c++23"

# Two distinct std identities on disk, one per level.
std20=$(grep -rl '"std_flag": "-std=c++20"' "$MCPP_HOME/build-cache/v1/std" 2>/dev/null | wc -l)
std23=$(grep -rl '"std_flag": "-std=c++23"' "$MCPP_HOME/build-cache/v1/std" 2>/dev/null | wc -l)
[[ "$std20" -ge 1 && "$std23" -ge 1 ]] || {
    find "$MCPP_HOME/build-cache/v1/std" -name std-module.json \
        -exec grep -H '"std_flag"' {} \; 2>/dev/null
    echo "FAIL: expected a std BMI for each level (c++20=$std20 c++23=$std23)"
    exit 1
}

# Two distinct fingerprint dirs (target/<triple>/<fingerprint>/) — the level is
# a fingerprint axis, so the two builds cannot share a product directory.
dirs=$(find target -mindepth 2 -maxdepth 2 -type d | wc -l)
[[ "$dirs" -ge 2 ]] || {
    find target -mindepth 2 -maxdepth 2 -type d
    echo "FAIL: expected separate target dirs per standard, found $dirs"
    exit 1
}

echo "PASS: c++20 and c++23 keep separate std BMIs and target dirs"
