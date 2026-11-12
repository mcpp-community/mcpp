#!/usr/bin/env bash
# requires: llvm
# 1077 -- a program of another system is built, not run: `mcpp run` starts
# nothing it cannot observe (SPEC-016 §6; design 2026-11-12 C9, D25).
#
# A row's artifacts run on its execution system. Where that is not this
# machine's system and nothing states a runner, the artifact is not started:
# this machine's kernel maps a static program of its own architecture without
# reading the system it names, and gives that system's call numbers its own
# meaning. The answer is given before anything is spawned, by one predicate for
# built-in rows and for rows a platform defines.
#
# Criteria: on a host whose system is not FreeBSD, `mcpp run --target
# <arch>-freebsd-musl` builds the program, exits 2 without running it, states
# "built, not run", names both systems and prints the `[target.<triple>]`
# runner declaration for the row; `mcpp test` of the same target reports the
# tests as built, not run.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; tail -30 "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in x86_64|amd64) ARCH=x86_64 ;; *) ARCH=aarch64 ;; esac
T="$ARCH-freebsd-musl"

cd "$TMP"
"$MCPP" new app > /dev/null 2>&1
cd app
"$MCPP" build --target "$T" > b.log 2>&1 || fail "the static FreeBSD program did not build" b.log
exe=$(find "target/$T" -path '*/bin/app*' -type f | head -1)
[ -n "$exe" ] || fail "no program" b.log

# A marker the program would write if it were started: the program of `mcpp
# new` prints, so its output in the log is the evidence of a launch.
set +e
"$MCPP" run --target "$T" > run.log 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "run exited $rc, expected the refusal's 2" run.log
grep -q "built, not run" run.log || fail "the run does not say built, not run" run.log
grep -q "artifacts run on freebsd" run.log || fail "the artifact's system is not named" run.log
grep -q "\[target.$T\] runner = " run.log || fail "the runner declaration does not name the row" run.log
grep -q "Running" run.log && fail "the artifact was started" run.log

# `--no-runner` states that the triple is native here, and is believed: the
# launch is then the caller's, and the refusal above does not apply.
set +e
"$MCPP" run --target "$T" --no-runner > direct.log 2>&1
set -e
grep -q "built, not run" direct.log && fail "--no-runner was not believed" direct.log

echo "PASS: 1077 a program of another system is built, not run"
