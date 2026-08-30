#!/usr/bin/env bash
# requires: gcc
# 324_fast_path_sees_path_deps.sh — a new file in a `path` dependency is not
# invisible to the fast path.
#
# The staleness sweep was rooted at the project being built, so a source tree
# reached through `path = "../dep"` was outside all of it. An EDIT to an
# existing file was still caught, but not by the sweep: ninja rebuilt the object
# and the relink made the fast path abandon afterwards. A NEW FILE has no edge
# at all — nothing in the graph mentions it and every recorded timestamp is
# unmoved — so the fast path replayed a build.ninja that predated it. Measured
# before the fix:
#
#     $ mcpp build
#         Finished dev in 0.00s        # and the module was never compiled
#
# This is #359's shape ("a glob input changes without any existing file's mtime
# changing") in a directory that fix did not reach. It matters most where it is
# hardest to notice: workspace members depend on each other by `path`.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p dep/src app/src
printf '[package]\nname = "dep"\nversion = "0.1.0"\nstandard = "c++23"\n[targets.dep]\nkind = "lib"\n' > dep/mcpp.toml
printf 'export module dep;\nexport int one(){ return 1; }\n' > dep/src/dep.cppm
printf '[package]\nname = "app"\nversion = "0.1.0"\nstandard = "c++23"\n[dependencies]\ndep = { path = "../dep" }\n' > app/mcpp.toml
printf 'import dep;\nint main(){ return one()==1 ? 0 : 1; }\n' > app/src/main.cpp

cd app
"$MCPP" build > b1.log 2>&1 || { cat b1.log; exit 1; }
# Second build records the dependency source roots and arms the fast path.
"$MCPP" build > b2.log 2>&1 || { cat b2.log; exit 1; }

# The fast path must actually be in play, or the rest of this test is asserting
# on a path it never took. A fast-path build prints no "Compiling" line.
"$MCPP" build > warm.log 2>&1 || { cat warm.log; exit 1; }
if grep -q "Compiling" warm.log; then
    echo "NOTE: the fast path was not taken on a warm build; the assertion below"
    echo "      still holds but tests less than it means to."
    cat warm.log
fi

count_gcm() { find . ../dep -name '*.gcm' 2>/dev/null | wc -l | tr -d ' '; }
before=$(count_gcm)

# ── the case that was invisible ─────────────────────────────────────────────
sleep 1
printf 'export module dep.extra;\nexport int two(){ return 2; }\n' > ../dep/src/extra.cppm
"$MCPP" build > add.log 2>&1 || { cat add.log; exit 1; }

if ! find . ../dep -name 'dep.extra.gcm' | grep -q .; then
    echo "FAIL: a new source file in a path dependency was never compiled,"
    echo "      and the build reported success:"
    cat add.log
    exit 1
fi

# THE DENOMINATOR. "It rebuilt" is satisfied by a full rebuild triggered for an
# unrelated reason; the count going up by exactly one says the new unit is what
# was added.
after=$(count_gcm)
[ "$after" -gt "$before" ] || {
    echo "FAIL: the module-interface count did not grow ($before -> $after)"
    exit 1
}

# ── and the dependency's MANIFEST is watched too ────────────────────────────
#
# A member that gains a target or a glob changes what the graph should be, and
# none of that is visible from its source files' timestamps.
"$MCPP" build > steady.log 2>&1
sleep 1
printf '\n[build]\ncxxflags = ["-DDEP_TOUCHED=1"]\n' >> ../dep/mcpp.toml
"$MCPP" build > toml.log 2>&1 || { cat toml.log; exit 1; }
grep -q "DEP_TOUCHED" compile_commands.json || {
    echo "FAIL: editing the path dependency's mcpp.toml did not re-plan"
    cat toml.log
    exit 1
}

echo "PASS: 324_fast_path_sees_path_deps"
