#!/usr/bin/env bash
# requires: unix-shell
# 878_an_upgraded_engine_does_not_replay_the_old_graph.sh -- #757.
#
# build.ninja names the engine that wrote it by absolute path: the `$mcpp` rules
# (dyndep, stage, the BMI schedule) and the `__action` wrapper start that
# executable. The emitter assumed that a new engine plans the graph again, "the
# version is in the fingerprint", and no fast path computes a fingerprint: each
# matched the recorded entry by target, profile, cache mode, features and
# toolchain request, and replayed it. After an upgrade that removed the previous
# install, `mcpp build` ran the old graph and every `stage` action failed
# because the program it names was gone; with the old install still present the
# new front end drove the actions of an older engine without saying so.
#
# The record of a build now carries the engine that wrote it, its version and the
# path of its executable (the path the graph uses), and one predicate
# (`admit_recorded_build`) compares it for every fast path.
#
# One program is built with an engine copied to one path, and the same binary is
# then run from another path, which is what a reinstall into another directory
# looks like to the record (the version is unchanged, so the path is what
# differs, and it is the part that breaks the graph).
#
#   A  the engine is moved and nothing else changes: the build succeeds, plans
#      the graph again, and no command of the new graph names the old path;
#   B  after that one build the next is replayed by the fast path again, and a
#      second move together with an edit of a deployed file (an action that
#      starts the engine has to run) succeeds with the new content in place;
#   C  a record written before the engine was recorded declines once (under -v
#      the reason is printed), and the build after it is replayed;
#   D  `mcpp run` is asked the same question, and runs the program;
#   E  a graph that another engine rewrote while the record still names this
#      one (what `--configure-only` from another install does) is not replayed:
#      the graph's own `$mcpp` binding is asked as well.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"
export NO_COLOR=1
# The engine is copied out of the directory a home would be derived from, so the
# home the build uses is stated rather than found from the copy's location.
export MCPP_HOME="${MCPP_HOME:-$HOME/.mcpp}"

EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac

replayed() {   # replayed <log>: the build was served by the fast path
    ! grep -q "declined" "$1" && ! grep -q "Resolving toolchain" "$1"
}
names() {      # names <dir>: does a graph under target/ name the directory
    grep -rl --include=build.ninja -F -- "$1" target 2>/dev/null | grep -q .
}

mkdir -p "$TMP/engine-one" "$TMP/engine-two" "$TMP/engine-three" "$TMP/app/src" "$TMP/app/assets"
cp "$MCPP" "$TMP/engine-one/mcpp$EXE"
cd "$TMP/app"
cat > mcpp.toml <<'TOML'
[package]
name    = "upgrade878"
version = "0.1.0"

[runtime]
deploy = [ { from = "assets/data.txt", to = "." } ]
TOML
printf '#include <cstdio>\nint main() { std::puts("hello"); return 0; }\n' > src/main.cpp
printf 'one\n' > assets/data.txt
deployed() { find target -name data.txt -path '*/bin/*' | head -1; }

ONE="$TMP/engine-one/mcpp$EXE"
"$ONE" build > b0.log 2>&1 || fail "the first build failed" b0.log
"$ONE" build -v > b1.log 2>&1 || fail "the warm build failed" b1.log
replayed b1.log || fail "the warm build was not replayed, so nothing below is measured against the fast path" b1.log
names "engine-one" || fail "the graph does not name the engine that wrote it, so the criterion has nothing to detect" b0.log
grep -q '^engine=' target/.build_cache || fail "the record does not name the engine that wrote it" target/.build_cache

# A. The engine moves; no source, manifest or deployed file changes.
mv "$TMP/engine-one/mcpp$EXE" "$TMP/engine-two/mcpp$EXE"
TWO="$TMP/engine-two/mcpp$EXE"
"$TWO" build -v > a1.log 2>&1 || fail "A: the build with the engine at its new path failed" a1.log
names "engine-one" && fail "A: a graph still names the path the engine was moved from" a1.log
names "engine-two" || fail "A: the regenerated graph does not name the engine that built it" a1.log
grep -q "fast-path: build declined" a1.log || fail "A: the build did not say that the fast path declined" a1.log

# B. Replayed again, and an action that starts the engine runs.
"$TWO" build -v > b2.log 2>&1 || fail "B: the build after the regeneration failed" b2.log
replayed b2.log || fail "B: the build after the regeneration was not replayed by the fast path" b2.log
mv "$TMP/engine-two/mcpp$EXE" "$TMP/engine-three/mcpp$EXE"
THREE="$TMP/engine-three/mcpp$EXE"
sleep 1.1
printf 'two\n' > assets/data.txt
"$THREE" build > b3.log 2>&1 || fail "B: the build after the second move and an edit of a deployed file failed" b3.log
[ "$(cat "$(deployed)")" = "two" ] || fail "B: the edit of the deployed file did not reach the output directory" b3.log
names "engine-two" && fail "B: a graph still names the path the engine was moved from"

# C. A record written before the engine was recorded declines once.
grep -v '^engine=' target/.build_cache > target/.build_cache.aged
mv target/.build_cache.aged target/.build_cache
"$THREE" build -v > c1.log 2>&1 || fail "C: the build with an aged record failed" c1.log
grep -q "predates the engine identity" c1.log || fail "C: the decline did not name the missing engine identity" c1.log
grep -q '^engine=' target/.build_cache || fail "C: the build did not record the engine" target/.build_cache
"$THREE" build -v > c2.log 2>&1 || fail "C: the build after the aged record failed" c2.log
replayed c2.log || fail "C: the record written after the decline was not replayed" c2.log

# D. `mcpp run`, whose fast path is a different function, asks the same question.
mv "$TMP/engine-three/mcpp$EXE" "$TMP/engine-one/mcpp$EXE"
"$ONE" run -v > d1.log 2>&1 || fail "D: the run with the engine at another path failed" d1.log
grep -q '^hello$' d1.log || fail "D: the program did not run" d1.log
grep -q "fast-path: run declined" d1.log || fail "D: the run did not say that the fast path declined" d1.log
names "engine-three" && fail "D: a graph still names the path the engine was moved from"

# E. The graph names its engine itself. Another engine rewrote the graph and
#    left the record: the fast path asks the graph, declines, and plans again.
"$ONE" build > e0.log 2>&1 || fail "E: the build before the rewrite failed" e0.log
for g in $(find target -name build.ninja -not -path '*/.build-mcpp/*'); do
    awk -v p="$TMP/engine-gone/mcpp$EXE" '/^mcpp *=/ { print "mcpp      = " p; next } { print }' \
        "$g" > "$g.rewritten" && mv "$g.rewritten" "$g"
done
names "engine-gone" || fail "E: the rewrite did not take"
"$ONE" build -v > e1.log 2>&1 || fail "E: the build of a graph another engine wrote failed" e1.log
grep -q "runs another engine" e1.log || fail "E: the decline did not name the graph's engine" e1.log
names "engine-gone" && fail "E: a graph still names the engine that rewrote it"

echo "OK"
