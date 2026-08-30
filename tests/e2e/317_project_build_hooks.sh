#!/usr/bin/env bash
# requires:
# Project-level build hooks (#496): lifecycle order, policy, timeout, the
# directory a hook runs in, and the two places nothing may fire.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/app/src" "$TMP/app/nested"

cat > "$TMP/app/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF

write_manifest() {
    cat > "$TMP/app/mcpp.toml"
}

# cmd.exe writes CRLF and /bin/sh writes LF, so the line ENDINGS are the host's
# and the line CONTENT is the hook's. Compare the content.
#   expect_log <file> <line>...
expect_log() {
    local file=$1; shift
    local want got
    want=$(printf '%s\n' "$@")
    got=$(tr -d '\r' < "$file" 2>/dev/null || true)
    if [[ "$got" != "$want" ]]; then
        echo "FAIL: $file"
        echo "--- expected ---"; printf '%s\n' "$want"
        echo "--- actual ---";   printf '%s\n' "$got"
        exit 1
    fi
}

# A WHOLE line, matched as one: a substring would still pass if the message
# were reworded around it. No pipe into grep — a `grep -q` that exits on the
# first match SIGPIPEs its producer, which reads as failure the moment anyone
# adds `set -o pipefail` to this file.
expect_line() {   # expect_line <file> <exact whole line>
    local file=$1 line=$2
    local body
    body=$(tr -d '\r' < "$file" 2>/dev/null || true)
    if [[ $'\n'"$body"$'\n' != *$'\n'"$line"$'\n'* ]]; then
        cat "$file"
        echo "FAIL: '$line' is not a line of $file"
        exit 1
    fi
}

# ── The success lifecycle, twice ─────────────────────────────────────────
#
# Twice on purpose: the second `mcpp build` is the one that would otherwise
# take the no-op fast path, which skips preparation and would skip the hooks
# with it. A single run cannot tell the two paths apart.
write_manifest <<'EOF'
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = "echo start>>hooks.log"
build_failed = "echo failed>>hooks.log"
build_finished = "echo finished>>hooks.log"
timeout_seconds = 10
enabled = true
side_effect = false
EOF

# Invoked BELOW the project root: a hook's relative paths belong to the root,
# not to wherever the user happened to type `mcpp build`.
cd "$TMP/app/nested"
"$MCPP" build > success-1.log 2>&1 || {
    cat success-1.log; echo "FAIL: hooked build failed"; exit 1; }
"$MCPP" build > success-2.log 2>&1 || {
    cat success-2.log; echo "FAIL: second hooked build failed"; exit 1; }
cd "$TMP/app"
[[ ! -e nested/hooks.log ]] || {
    echo "FAIL: hook ran in the invocation directory, not the project root"; exit 1; }
expect_log hooks.log start finished start finished

# ── A compiler failure chooses build_failed, never build_finished ────────
cat > src/main.cpp <<'EOF'
#error "intentional hook failure path"
int main() { return 0; }
EOF
: > hooks.log
rc=0
"$MCPP" build > build-failed.log 2>&1 || rc=$?
[[ $rc -ne 0 ]] || { cat build-failed.log; echo "FAIL: broken source built"; exit 1; }
expect_log hooks.log start failed

cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF

# ── A failing hook is REPORTED and the build keeps its own result ───────
#
# While `[hooks]` is experimental it may not decide whether a build succeeded.
# The build below succeeds with a hook that cannot run at all, and the two
# assertions are both needed: the exit code says the hook had no vote, and the
# warning says it was not silently swallowed.
write_manifest <<'EOF'
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_finished = "mcpp-hook-command-that-does-not-exist"
EOF
"$MCPP" build > ignored-hook-failure.log 2>&1 || {
    cat ignored-hook-failure.log
    echo "FAIL: a failing hook changed the build result"; exit 1; }
grep -q "warning: hook 'build_finished' exited with status" ignored-hook-failure.log || {
    cat ignored-hook-failure.log; echo "FAIL: the hook failure was not reported"; exit 1; }
# `if grep` rather than `grep && { }`: a NEGATIVE assertion written with `&&`
# leaves the list's exit status as grep's, and reading that under `set -e` is
# an argument this file should not be having.
if grep -q "error: hook 'build_finished'" ignored-hook-failure.log; then
    cat ignored-hook-failure.log
    echo "FAIL: an experimental hook failure was reported as an error"; exit 1
fi

# The explicit spelling of the current behaviour keeps working, so a manifest
# does not have to change when the feature is promoted.
write_manifest <<'EOF'
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_finished = "mcpp-hook-command-that-does-not-exist"
side_effect = false
EOF
"$MCPP" build > explicit-false.log 2>&1 || {
    cat explicit-false.log; echo "FAIL: side_effect=false was not accepted"; exit 1; }

# ── `side_effect = true` is REFUSED, not quietly downgraded ─────────────
#
# Honouring it would give an experimental feature a veto over every build;
# ignoring it would leave the project believing its build is gated on a
# notifier when nothing is. Both silent options are worse than an error.
write_manifest <<'EOF'
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_finished = "echo done"
side_effect = true
EOF
rc=0
"$MCPP" build > side-effect-true.log 2>&1 || rc=$?
[[ $rc -ne 0 ]] || {
    cat side-effect-true.log; echo "FAIL: side_effect=true was accepted"; exit 1; }
grep -q "experimental" side-effect-true.log || {
    cat side-effect-true.log
    echo "FAIL: the refusal does not say why"; exit 1; }

# ── enabled = false switches off a table that still names commands ──────
write_manifest <<'EOF'
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = "echo disabled>>disabled.log"
build_finished = "echo disabled>>disabled.log"
enabled = false
EOF
rm -f disabled.log
"$MCPP" build > disabled.log.out 2>&1 || {
    cat disabled.log.out; echo "FAIL: disabled hook build failed"; exit 1; }
[[ ! -e disabled.log ]] || {
    cat disabled.log; echo "FAIL: enabled=false still ran hooks"; exit 1; }

# ── The timeout is a real bound, on both host shells ────────────────────
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) HOST_WINDOWS=1 ;;
    *)                    HOST_WINDOWS=0 ;;
esac
if [[ $HOST_WINDOWS -eq 1 ]]; then
    slow_command="ping -n 6 127.0.0.1 >NUL"
    pause_2s="ping -n 3 127.0.0.1 >NUL"
else
    slow_command="sleep 5"
    pause_2s="sleep 2"
fi
write_manifest <<EOF
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_finished = "$slow_command"
timeout_seconds = 1
EOF
"$MCPP" build > timeout.log 2>&1 || {
    cat timeout.log; echo "FAIL: a timed-out hook changed the build result"; exit 1; }
# The bound is real even though the build survives it: without the timeout the
# command would have run for five seconds, and the line below would be absent.
expect_line timeout.log "warning: hook 'build_finished' timed out after 1s"

# ── An invalid value fails the manifest, before any command runs ────────
#
# And it fails as a MANIFEST error naming the key — `[hooks]` is a section of
# mcpp.toml, so a bad value there is reported by the same parser and in the
# same words as a bad value anywhere else in the file.
write_manifest <<'EOF'
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = "echo should-not-run>>invalid.log"
timeout_seconds = 0
EOF
rm -f invalid.log
rc=0
"$MCPP" build > invalid-config.log 2>&1 || rc=$?
[[ $rc -ne 0 ]] || { cat invalid-config.log; echo "FAIL: invalid hooks were accepted"; exit 1; }
[[ ! -e invalid.log ]] || { cat invalid.log; echo "FAIL: invalid hook ran"; exit 1; }
grep -q "\[hooks\].timeout_seconds must be a positive integer" invalid-config.log || {
    cat invalid-config.log; echo "FAIL: invalid hook diagnostic is missing"; exit 1; }

# ── An unknown key is a warning, and the known ones still run ───────────
: > hooks.log
write_manifest <<'EOF'
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = "echo start>>hooks.log"
build_finsihed = "echo typo>>hooks.log"
EOF
"$MCPP" build > unknown-key.log 2>&1 || {
    cat unknown-key.log; echo "FAIL: unknown hook key was fatal"; exit 1; }
grep -q "\[hooks\] has unsupported key 'build_finsihed'" unknown-key.log || {
    cat unknown-key.log; echo "FAIL: unknown hook key was not reported"; exit 1; }
expect_log hooks.log start

# ── Preparation failing fires NOTHING ───────────────────────────────────
#
# The lifecycle is paired: build_finished/build_failed are only reachable after
# build_start has run. A project that cannot be prepared has not started
# building — and its hook program may be exactly what preparation would have
# installed, so "report the failure" is not available here.
: > hooks.log
write_manifest <<'EOF'
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = "echo start>>hooks.log"
build_failed = "echo failed>>hooks.log"

[dependencies]
absent = { path = "no/such/dependency" }
EOF
rc=0
"$MCPP" build > prepare-failed.log 2>&1 || rc=$?
[[ $rc -ne 0 ]] || {
    cat prepare-failed.log; echo "FAIL: a missing path dependency built"; exit 1; }
expect_log hooks.log

# ── `during_build`: an interval closed by the build, not by the command ──
#
# The assertions below are about STATE, never about a log line. "mcpp said it
# stopped the command" passes whether or not anything stopped; "the file it was
# appending to has not grown in a second" does not.
if [[ $HOST_WINDOWS -eq 1 ]]; then
    cat > writer.cmd <<'EOF'
@echo off
:loop
echo tick>>beat.log
ping -n 2 127.0.0.1 >NUL
goto loop
EOF
    # `start /b` returns immediately, so the command mcpp starts EXITS and the
    # writer is left behind it — the job object is the only thing that can
    # still reach it.
    writer_with_grandchild="start /b cmd /c writer.cmd"
else
    cat > writer.sh <<'EOF'
while :; do echo tick>>beat.log; sleep 0.2; done
EOF
    # The shell mcpp starts forks the writer and waits, so the writer is a
    # GRANDCHILD. This is the case `kill(pid)` misses and `killpg` catches, and
    # it is the only assertion that tells the fix from the bug.
    writer_with_grandchild="sh writer.sh & wait"
fi

beat_is_frozen() {   # the file grew, and then stopped growing
    local before after
    before=$(wc -c < beat.log 2>/dev/null || echo 0)
    sleep 1
    after=$(wc -c < beat.log 2>/dev/null || echo 0)
    [[ "$before" != "0" && "$before" == "$after" ]]
}

: > hooks.log
rm -f beat.log
write_manifest <<EOF
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = "$pause_2s"
build_finished = "echo finished>>hooks.log"
during_build = "$writer_with_grandchild"
EOF
"$MCPP" build > during-build.log 2>&1 || {
    cat during-build.log; echo "FAIL: build with during_build failed"; exit 1; }
beat_is_frozen || {
    echo "FAIL: during_build kept running after the build (or never ran)"
    wc -c beat.log 2>/dev/null; exit 1; }
# The interval closed BEFORE the terminal hook, which is the ordering that
# keeps a "build finished" sound from playing over the music it replaces.
expect_log hooks.log finished

# ── `loop` restarts a command that ends before its interval does ────────
rm -f runs.log
write_manifest <<EOF
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = "$pause_2s"
during_build = { cmd = "echo run>>runs.log", loop = true }
EOF
"$MCPP" build > loop-on.log 2>&1 || {
    cat loop-on.log; echo "FAIL: looped during_build failed the build"; exit 1; }
looped=$(wc -l < runs.log | tr -d ' ')
[[ "$looped" -ge 2 ]] || {
    cat loop-on.log; echo "FAIL: loop=true ran $looped time(s), expected >= 2"; exit 1; }

# Without `loop`, the same command runs exactly once — the denominator that
# makes the count above mean something.
rm -f runs.log
write_manifest <<EOF
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = "$pause_2s"
during_build = "echo run>>runs.log"
EOF
"$MCPP" build > loop-off.log 2>&1 || {
    cat loop-off.log; echo "FAIL: unlooped during_build failed the build"; exit 1; }
once=$(wc -l < runs.log | tr -d ' ')
[[ "$once" -eq 1 ]] || {
    cat loop-off.log; echo "FAIL: loop=false ran $once time(s), expected 1"; exit 1; }

# ── A command that cannot stay up stops, rather than spinning ───────────
#
# `loop` on a typo is a fork bomb otherwise. The bound is five consecutive
# runs that end unsuccessfully within a second.
write_manifest <<EOF
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = "$pause_2s"
during_build = { cmd = "mcpp-hook-command-that-does-not-exist", loop = true }
EOF
# The build still succeeds — an experimental hook has no vote — but giving up
# has to be VISIBLE, or a silent spin and a silent stop look identical.
"$MCPP" build > loop-giveup.log 2>&1 || {
    cat loop-giveup.log
    echo "FAIL: a during_build that never ran changed the build result"; exit 1; }
grep -q "warning: hook 'during_build' failed to stay up" loop-giveup.log || {
    cat loop-giveup.log; echo "FAIL: giving up was not reported"; exit 1; }

# ── `loop` on a self-closing event is refused, not ignored ──────────────
write_manifest <<'EOF'
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = { cmd = "echo start", loop = true }
EOF
rc=0
"$MCPP" build > loop-misplaced.log 2>&1 || rc=$?
[[ $rc -ne 0 ]] || {
    cat loop-misplaced.log; echo "FAIL: loop on build_start was accepted"; exit 1; }
grep -q "during_build" loop-misplaced.log || {
    cat loop-misplaced.log
    echo "FAIL: the diagnostic does not name the event that supports loop"; exit 1; }

# ── An interrupted build leaves nothing running ─────────────────────────
#
# POSIX only, and the skip is printed rather than silent: sending Ctrl-C to
# another process on Windows needs a helper this suite does not have, and a
# test that cannot fail there would read as coverage.
if [[ $HOST_WINDOWS -eq 1 ]]; then
    echo "SKIP(within test): Ctrl-C cleanup — no way to signal another process here"
else
    rm -f beat.log
    write_manifest <<EOF
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = "sleep 20"
during_build = "$writer_with_grandchild"
EOF
    "$MCPP" build > interrupted.log 2>&1 &
    build_pid=$!
    sleep 3
    kill -INT "$build_pid" 2>/dev/null || true
    wait "$build_pid" 2>/dev/null || true
    beat_is_frozen || {
        echo "FAIL: Ctrl-C left during_build running"; exit 1; }
fi

# ── A DEPENDENCY's hooks never run ──────────────────────────────────────
#
# Every manifest mcpp parses carries a `[hooks]` field, a dependency's
# included. Only the root project's is ever invoked, and that is the whole
# reason `mcpp add` of a third-party package is not "run their shell command
# on my next build". A comment cannot hold that; this can.
DEP="$TMP/dep"
DEP_HOST="$(host_path "$DEP")"
mkdir -p "$DEP/src"
cat > "$DEP/src/dep.cppm" <<'EOF'
export module dep;
export int dep_answer() { return 0; }
EOF
cat > "$DEP/mcpp.toml" <<'EOF'
[package]
name = "dep"
version = "0.1.0"

[targets.dep]
kind = "lib"

[hooks]
build_start = "echo dependency>>dependency-hook.log"
build_finished = "echo dependency>>dependency-hook.log"
EOF
: > hooks.log
rm -f "$DEP/dependency-hook.log" dependency-hook.log
write_manifest <<EOF
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_start = "echo start>>hooks.log"
build_finished = "echo finished>>hooks.log"

[dependencies]
dep = { path = "$DEP_HOST" }
EOF
cat > src/main.cpp <<'EOF'
import dep;
int main() { return dep_answer(); }
EOF
"$MCPP" build > dependency-hooks.log 2>&1 || {
    cat dependency-hooks.log; echo "FAIL: build with a hooked dependency failed"; exit 1; }
expect_log hooks.log start finished
[[ ! -e "$DEP/dependency-hook.log" && ! -e dependency-hook.log ]] || {
    echo "FAIL: a dependency's hooks ran"; exit 1; }

# ── A workspace runs each MEMBER's hooks, in that member's root ─────────
#
# Members are separate packages, so their hooks are separate too — and the
# workspace root's own `[hooks]` belongs to a node that builds nothing.
WS="$TMP/ws"
mkdir -p "$WS/alpha/src" "$WS/beta/src"
cat > "$WS/mcpp.toml" <<'EOF'
[workspace]
members = ["alpha", "beta"]

[hooks]
build_start = "echo root>>root.log"
build_finished = "echo root>>root.log"
EOF
for member in alpha beta; do
    cat > "$WS/$member/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF
    cat > "$WS/$member/mcpp.toml" <<EOF
[package]
name = "$member"
version = "0.1.0"

[hooks]
build_start = "echo $member-start>>hooks.log"
build_finished = "echo $member-finished>>hooks.log"
EOF
done
cd "$WS"
"$MCPP" build > ws-build.log 2>&1 || {
    cat ws-build.log; echo "FAIL: workspace build with hooks failed"; exit 1; }
expect_log alpha/hooks.log alpha-start alpha-finished
expect_log beta/hooks.log beta-start beta-finished
[[ ! -e root.log ]] || {
    cat root.log; echo "FAIL: the workspace root's own hooks fired"; exit 1; }
[[ ! -e hooks.log ]] || {
    cat hooks.log; echo "FAIL: a member hook ran in the workspace root"; exit 1; }

echo "OK"
