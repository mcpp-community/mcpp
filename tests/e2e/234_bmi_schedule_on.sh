#!/usr/bin/env bash
# `bmi_schedule = "on"` end to end, and the token leak that used to hang it.
#
# The split schedule reorders the module graph: a BMI edge that exits as soon as
# the compiler has published its BMI, plus a join edge that waits for code
# generation. Until now the only coverage was a unit test of `decide()` — which
# checks the TABLE, not the mechanism. Nothing built a project with the feature
# on, so every hazard in detach_codegen.cppm was guarded by review alone.
#
# Two contracts here, and the second is the one that bit:
#
#  1. A build with the feature on produces a working binary, and the graph says
#     it is split. If the strategy silently fell back, the timings would look
#     like a regression in the feature rather than like the feature being off.
#
#  2. ⚠️ STALE CONCURRENCY TOKENS MUST BE RECLAIMED. Real compiler concurrency is
#     bounded by a semaphore of directories under `<build>/.mcpp-sched`, and a
#     token is removed by the supervisor holding it. A supervisor that never runs
#     its cleanup — Ctrl-C on the build, the OOM killer, a reboot — leaves its
#     directory behind, and nothing used to remove it. Each such event lowered
#     the cap for that build directory PERMANENTLY, and after `cap` of them the
#     next build waited for a token that could never be released: no output, no
#     diagnostic, forever. Interrupting a build is an ordinary thing to do.
#
#     This test plants a full set of stale tokens and requires the next build to
#     finish anyway.
set -e

_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -z "${MCPP:-}" ]; then
  MCPP="$(bash "$_root/.github/tools/newest_artifact.sh" "$_root" mcpp 2>/dev/null || true)"
  [ -n "$MCPP" ] || { echo "SKIP: no mcpp binary built yet — run \`mcpp build\` first"; exit 0; }
fi
# ⚠️ ABSOLUTISE WHATEVER WE GOT, not just what we derived. This test `cd`s into a
# temp directory, so a RELATIVE MCPP — which is what `ls -t target/*/*/bin/mcpp`
# hands you, and what a caller naturally exports — stops resolving the moment we
# leave the repository, with
#     line 77: target/.../bin/mcpp: No such file or directory
# reported as "the build with bmi_schedule=on failed". The normalisation used to
# live inside the `if`, so it ran only when the test found the binary itself.
case "$MCPP" in /*) ;; *) MCPP="$_root/$MCPP" ;; esac
[ -x "$MCPP" ] || { echo "FAIL: MCPP=$MCPP is not executable"; exit 1; }
export MCPP

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ⚠️ PIN PER PLATFORM, not just `default`. A bare `default = "gcc@16.1.0"` sends
# macOS and Windows looking for a gcc payload that does not exist for them, and
# the whole test dies at step 1 with `'xim:gcc@16.1.0' not in current index` —
# reported as "the build with bmi_schedule=on failed", which is a completely
# different diagnosis. Same three-line block every other e2e in this directory
# uses.
cat > mcpp.toml <<'EOF'
[package]
name    = "schedon"
version = "0.1.0"

[toolchain]
default = "gcc@16.1.0"
macos   = "llvm@22.1.8"
windows = "llvm@20.1.7"

[build]
bmi_schedule = "on"
EOF

mkdir -p src
cat > src/core.cppm <<'EOF'
export module schedon.core;
import std;
export int core_value() { return 7; }
EOF
cat > src/mid.cppm <<'EOF'
export module schedon.mid;
import std;
import schedon.core;
export int mid_value() { return core_value() + 1; }
EOF
cat > src/main.cpp <<'EOF'
import std;
import schedon.mid;
int main() { std::println("{}", mid_value()); return mid_value() == 8 ? 0 : 1; }
EOF

# detach-codegen is the GCC strategy. On a machine whose default toolchain
# resolves to clang the decision is TwoPhase and there is no semaphore at all,
# so the second contract has nothing to test — say so rather than pass hollowly.
say_skip_reason() { echo "SKIP: $1"; exit 0; }

echo "== 1. builds with bmi_schedule = \"on\" =="
"$MCPP" build --release --verbose > build.log 2>&1 || {
  echo "FAIL: build with bmi_schedule=on failed"; tail -30 build.log; exit 1;
}

grep -q 'schedule:' build.log || {
  echo "FAIL: --verbose printed no schedule line — the decision is not being reported"
  tail -20 build.log; exit 1
}
sched_line=$(grep -m1 'schedule:' build.log)
echo "  $sched_line"

case "$sched_line" in
  *detach-codegen*) strategy=detach ;;
  *two-phase*)      strategy=twophase ;;
  *)  say_skip_reason "the schedule resolved to '${sched_line#*schedule: }' on this host; \
this test covers the split strategies" ;;
esac

BIN=$(ls -t target/*/*/bin/schedon 2>/dev/null | head -1)
[ -n "$BIN" ] || { echo "FAIL: no binary produced"; exit 1; }
out=$("$BIN")
[ "$out" = "8" ] || { echo "FAIL: binary printed '$out', expected 8"; exit 1; }
echo "  built and ran: $out"

BUILD_DIR=$(dirname "$(dirname "$BIN")")

echo "== 2. the graph declares its shape =="
# HAZARD 4 in detach_codegen.cppm: build.ninja is shared mutable state and the
# fast path replays it, so a split graph must say so about itself. Asserting the
# tag exists is what keeps a later fast-path change from replaying a split graph
# as if it were an ordinary one.
grep -q '^# mcpp:graph=' "$BUILD_DIR/build.ninja" || {
  echo "FAIL: build.ninja carries no '# mcpp:graph=' line"; exit 1; }
echo "  $(grep -m1 '^# mcpp:graph=' "$BUILD_DIR/build.ninja")"

if [ "$strategy" != detach ]; then
  echo "== 3. skipped: no semaphore under the $strategy strategy =="
  echo "234 bmi_schedule OK ($strategy)"
  exit 0
fi

echo "== 3. an incremental build still works =="
touch src/core.cppm
"$MCPP" build --release > inc.log 2>&1 || {
  echo "FAIL: incremental build after touching an interface failed"; tail -30 inc.log; exit 1; }
echo "  ok"

echo "== 4. a full set of STALE TOKENS does not hang the next build =="
SEM="$BUILD_DIR/.mcpp-sched"
mkdir -p "$SEM"
# Every slot taken, by nothing. Before the reclaim this made `acquire_token`
# wait forever on the first module edge.
for i in $(seq 0 63); do mkdir -p "$SEM/$i"; done
planted=$(ls "$SEM" | wc -l)
[ "$planted" -gt 0 ] || { echo "FAIL: could not plant tokens"; exit 1; }
echo "  planted $planted stale tokens"

# Force real work, so the build must actually take a token.
printf '\nexport int core_extra() { return 1; }\n' >> src/core.cppm

# A generous bound that is still far below the in-process 2h fallback: if the
# reclaim is gone this hangs, and the timeout is what turns that into a failure
# a CI log can explain.
# ⚠️ CAPTURE THE STATUS BEFORE ANY TEST TOUCHES IT. Inside `if ! cmd; then`,
# `$?` is the status of `! cmd` — which is 0 exactly when cmd FAILED — so a real
# failure got reported as "exited 0" and the timeout case could never be
# distinguished from any other. Same shape as reading `$?` after `cmd | tail`.
#
# `set -e` has to come off for exactly this command: with errexit on, a failing
# build aborts the script before `rc=$?` runs, which is what pushed the previous
# version into the `if !` form that then could not read the status at all.
set +e
timeout 600 "$MCPP" build --release > stale.log 2>&1
rc=$?
set -e
if [ "$rc" != 0 ]; then
  if [ "$rc" = 124 ]; then
    echo "FAIL: the build HUNG with a full set of stale tokens in $SEM"
    echo "      (the per-build reclaim in prepare.cppm is not running)"
  else
    echo "FAIL: build after planting stale tokens exited $rc"
  fi
  tail -30 stale.log
  exit 1
fi
echo "  build completed with stale tokens present"

left=$(ls "$SEM" 2>/dev/null | wc -l)
[ "$left" -lt "$planted" ] || {
  echo "FAIL: $left of $planted stale tokens survived — they were not reclaimed,"
  echo "      so the build only succeeded by not needing a slot"
  exit 1; }
echo "  reclaimed: $planted -> $left"

BIN=$(ls -t target/*/*/bin/schedon 2>/dev/null | head -1)
out=$("$BIN")
[ "$out" = "8" ] || { echo "FAIL: binary printed '$out' after the stale-token build"; exit 1; }

echo "234 bmi_schedule OK (detach-codegen, tokens reclaimed)"
