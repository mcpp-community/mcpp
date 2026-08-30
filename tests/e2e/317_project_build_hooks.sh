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
side_effect = true
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

# ── side_effect = false: reported, but the build keeps its own result ────
write_manifest <<'EOF'
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_finished = "mcpp-hook-command-that-does-not-exist"
side_effect = false
EOF
"$MCPP" build > ignored-hook-failure.log 2>&1 || {
    cat ignored-hook-failure.log
    echo "FAIL: side_effect=false changed build success"; exit 1; }
grep -q "warning: hook 'build_finished' exited with status" ignored-hook-failure.log || {
    cat ignored-hook-failure.log; echo "FAIL: ignored hook failure was not reported"; exit 1; }

# ── The same failure is fatal under the default side_effect = true ──────
write_manifest <<'EOF'
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_finished = "mcpp-hook-command-that-does-not-exist"
EOF
rc=0
"$MCPP" build > fatal-hook-failure.log 2>&1 || rc=$?
[[ $rc -ne 0 ]] || {
    cat fatal-hook-failure.log; echo "FAIL: fatal hook failure returned success"; exit 1; }
grep -q "error: hook 'build_finished' exited with status" fatal-hook-failure.log || {
    cat fatal-hook-failure.log; echo "FAIL: fatal hook failure was not reported"; exit 1; }

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
    MINGW*|MSYS*|CYGWIN*) slow_command="ping -n 6 127.0.0.1 >NUL" ;;
    *)                    slow_command="sleep 5" ;;
esac
write_manifest <<EOF
[package]
name = "hookprobe"
version = "0.1.0"

[hooks]
build_finished = "$slow_command"
timeout_seconds = 1
EOF
rc=0
"$MCPP" build > timeout.log 2>&1 || rc=$?
[[ $rc -ne 0 ]] || { cat timeout.log; echo "FAIL: timed-out hook returned success"; exit 1; }
expect_line timeout.log "error: hook 'build_finished' timed out after 1s"

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
