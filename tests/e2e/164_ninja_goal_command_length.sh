#!/usr/bin/env bash
# requires:
# 164_ninja_goal_command_length.sh — an explicit ninja goal set must travel
# through the manifest, never on the command line.
#
# `mcpp test` names every shared prerequisite as a goal so that a broken package
# source fails once as a package error instead of N times as identical per-test
# compile failures. For a large package that is thousands of object paths:
# FFmpeg's 2281 translation units produced a 50,781-character argv. Windows
# joins argv into one command string for cmd.exe, which caps at 8191 characters
# — the command never ran and a bare 127 came back, from cmd.exe, with no output
# from either ninja or mcpp. mcpp-index's Windows job failed this way on the
# ffmpeg member from 0.0.104 (which introduced explicit goals) onward.
#
# The fix routes the goal set into build.ninja as a phony edge and puts one word
# on the command line. This test asserts that mechanism rather than the 8191
# limit itself: reproducing the limit needs a package big enough to be far too
# slow here, and the limit does not exist on Linux/macOS at all. What is
# portable — and what actually regressed — is "goals go in the manifest".
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p proj/src proj/tests
cat > proj/mcpp.toml <<'EOF'
[package]
name    = "goalset"
version = "0.1.0"
EOF

# Enough units that a regression would be visible as a long argv, while still
# building in a couple of seconds.
for i in $(seq 1 30); do
    echo "int unit_$i() { return $i; }" > "proj/src/unit_$i.cpp"
done
echo 'int main() { return 0; }' > proj/src/main.cpp
cat > proj/tests/sanity.cpp <<'EOF'
extern int unit_1();
int main() { return unit_1() == 1 ? 0 : 1; }
EOF

cd proj
"$MCPP" test > test.log 2>&1 || { cat test.log; echo "FAIL: mcpp test"; exit 1; }
grep -q "sanity ... ok" test.log || { cat test.log; exit 1; }

NINJA=$(find target -name build.ninja | head -1)
[ -n "$NINJA" ] || { echo "FAIL: no build.ninja"; exit 1; }

# The phony edge is how the goal set reaches ninja.
grep -q "^build mcpp-requested-goals : phony " "$NINJA" || {
    echo "FAIL: explicit goals must be aggregated into a phony edge"
    grep -n "phony" "$NINJA" | head; exit 1; }

# And the goals themselves must not have been left on a command line. mcpp
# records the ninja program it ran in target/.build_cache; the goal set never
# belongs there either.
if [ -f target/.build_cache ]; then
    if grep -qE 'obj/.*obj/.*obj/' target/.build_cache; then
        echo "FAIL: goal paths leaked into the recorded ninja invocation"
        cat target/.build_cache; exit 1
    fi
fi

echo "PASS 164_ninja_goal_command_length"
