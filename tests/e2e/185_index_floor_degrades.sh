#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 185_index_floor_degrades.sh — an index cannot decide whether mcpp works.
#
# An index tree may declare a client-version floor (`index.toml` min_mcpp). Two
# behaviours around that floor had NO end-to-end coverage at all — only the pure
# predicate `floor_violation()` was unit-tested — which is why both were wrong
# for as long as the feature has existed:
#
#   1. A floor violation made every descriptor read return "nothing", which is
#      the same answer as "this package is not in this index". The build then
#      died on `E_NOT_FOUND ... wire address tried: ...` — naming neither the
#      version nor the floor. The cause was printed much earlier, and the line
#      that actually stopped the build pointed at addressing instead.
#
#   2. That same indistinguishable miss fed the refresh policy, which read it as
#      "the local index is missing something, go fetch" — so an unusable index
#      drove repeated refreshes of itself.
#
# Both assertions below are about what the user READS, because that is what was
# broken. `mcpp explain E0006` and the floor predicate were fine throughout.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

# ── two path indexes: one readable, one that demands a newer mcpp ────────────
make_index() {   # $1 = dir, $2 = min_mcpp ("" = no contract), $3 = pkg name
    local dir="$1" floor="$2" pkg="$3"
    mkdir -p "$dir/pkgs/${pkg:0:1}"
    if [[ -n "$floor" ]]; then
        cat > "$dir/index.toml" <<EOF
[index]
spec     = "1"
min_mcpp = "$floor"
EOF
    fi
    cat > "$dir/pkgs/${pkg:0:1}/$pkg.lua" <<EOF
package = {
    spec = "1",
    name = "$pkg",
    description = "fixture",
    licenses = {"MIT"},
    type = "package",
    xpm = { linux = { ["1.0.0"] = {
        url = "https://example.invalid/$pkg-1.0.0.tar.gz",
        sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
    } } },
    mcpp = {
        language = "c++23",
        sources = { "src/**/*.cppm" },
        targets = { ["$pkg"] = { kind = "lib" } },
        deps = {},
    },
}
EOF
}

GOOD_INDEX="$TMP/good-index"
NEW_INDEX="$TMP/too-new-index"
make_index "$GOOD_INDEX" ""            goodlib
make_index "$NEW_INDEX"  "9999.9.9.9"  newlib

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
cat > src/main.cpp <<'EOF'
import std;
int main() { std::println("ok"); return 0; }
EOF

# ── 1. an unusable index must say so — not "package not found" ───────────────
cat > mcpp.toml <<EOF
[package]
name = "floorproj"
version = "0.1.0"

[indices]
toonew = { path = "$NEW_INDEX" }

[dependencies]
"toonew.newlib" = "1.0.0"

[targets.floorproj]
kind = "bin"
main = "src/main.cpp"
EOF

set +e
"$MCPP" build > build.log 2>&1
rc=$?
set -e
[[ "$rc" != "0" ]] || { echo "FAIL: build unexpectedly succeeded"; cat build.log; exit 1; }

grep -q "E0006" build.log || {
    echo "FAIL: the failure never mentions E0006 — the user cannot tell that the"
    echo "      cause is an index that needs a newer mcpp."
    cat build.log
    exit 1
}

# THE assertion that was missing, and it has to be precise.
#
# "E0006 appears somewhere in the output" is NOT enough — it always did, and a
# test asserting only that passes on the broken build too (verified). What was
# broken is that the message which STOPS the build said only
#
#     error: dependency 'toonew.newlib': not found in local index at '...'
#
# i.e. it blamed the package, pointing the user at publication or naming, while
# the actual answer (upgrade mcpp) had scrolled past. So: assert on the LAST
# error, and assert on the specific text that ties the two together.
last_error="$(grep -E '^error:' build.log | tail -1)"
echo "$last_error" | grep -q "not found" && {
    # A bare not-found as the final word is the exact regression. It is only
    # acceptable if the cause is attached to it.
    grep -A 4 "$(echo "$last_error" | head -c 40)" build.log \
        | grep -qE "cannot read|E0006" || {
        echo "FAIL: the build stopped on a bare 'not found' with no mention of the"
        echo "      index that could not be read. That blames the package for a"
        echo "      problem whose fix is 'upgrade mcpp'."
        echo "--- output ---"
        cat build.log
        exit 1
    }
}
grep -q "this mcpp cannot read" build.log || {
    echo "FAIL: nothing in the output names the unreadable index as the cause."
    cat build.log
    exit 1
}
echo "PASS: an unusable index reports E0006 as the cause"

# ── 2. an unusable index must not take a healthy one down with it ────────────
# INV-2: the blast radius of a floor bump is the index that declared it.
cat > mcpp.toml <<EOF
[package]
name = "floorproj"
version = "0.1.0"

[indices]
toonew = { path = "$NEW_INDEX" }
good   = { path = "$GOOD_INDEX" }

[targets.floorproj]
kind = "bin"
main = "src/main.cpp"
EOF

set +e
"$MCPP" build > iso.log 2>&1
iso_rc=$?
set -e
[[ "$iso_rc" == "0" ]] || {
    echo "FAIL: a project that depends on NOTHING from the too-new index still"
    echo "      failed to build — an unusable index is not supposed to be"
    echo "      contagious."
    cat iso.log
    exit 1
}
echo "PASS: an unusable index does not break a build that does not need it"

# ── 3. the escape hatch still works ─────────────────────────────────────────
# It exists for debugging; if it ever stops working the only lever a blocked
# user has is gone.
cat > mcpp.toml <<EOF
[package]
name = "floorproj"
version = "0.1.0"

[indices]
toonew = { path = "$NEW_INDEX" }

[targets.floorproj]
kind = "bin"
main = "src/main.cpp"
EOF
MCPP_INDEX_FLOOR=ignore "$MCPP" build > ignore.log 2>&1 || {
    echo "FAIL: MCPP_INDEX_FLOOR=ignore no longer bypasses the floor"
    cat ignore.log
    exit 1
}
grep -q "E0006" ignore.log && {
    echo "FAIL: MCPP_INDEX_FLOOR=ignore still reported E0006"
    cat ignore.log
    exit 1
}
echo "PASS: MCPP_INDEX_FLOOR=ignore bypasses the floor"

echo "OK"
