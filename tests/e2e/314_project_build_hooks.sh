#!/usr/bin/env bash
# requires:
# Project-level build hooks: lifecycle order, policy, timeout and project cwd.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/app/src" "$TMP/app/nested"

cat > "$TMP/app/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF

write_manifest() {
    cat > "$TMP/app/mcpp.toml"
}

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

# Invoke below the project root: hook-relative paths still belong to the root.
cd "$TMP/app/nested"
"$MCPP" build > success-1.log 2>&1 || {
    cat success-1.log; echo "FAIL: hooked build failed"; exit 1; }
"$MCPP" build > success-2.log 2>&1 || {
    cat success-2.log; echo "FAIL: second hooked build failed"; exit 1; }
cd "$TMP/app"
[[ "$(cat hooks.log)" == $'start\nfinished\nstart\nfinished' ]] || {
    cat hooks.log; echo "FAIL: success lifecycle order/count is wrong"; exit 1; }

# A compiler failure chooses build_failed, never build_finished.
cat > src/main.cpp <<'EOF'
#error "intentional hook failure path"
int main() { return 0; }
EOF
: > hooks.log
rc=0
"$MCPP" build > build-failed.log 2>&1 || rc=$?
[[ $rc -ne 0 ]] || { cat build-failed.log; echo "FAIL: broken source built"; exit 1; }
[[ "$(cat hooks.log)" == $'start\nfailed' ]] || {
    cat hooks.log; echo "FAIL: failure lifecycle is wrong"; exit 1; }

cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF

# A non-side-effecting hook reports its failure but preserves build success.
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

# The same hook failure is fatal with the default side_effect=true.
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

# Disabled hooks do nothing, including not forcing a command to run.
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

# A positive timeout is enforced by the platform process runner.
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
grep -q "hook 'build_finished' timed out after 1s" timeout.log || {
    cat timeout.log; echo "FAIL: hook timeout was not reported"; exit 1; }

# Invalid hook values fail before invoking any command.
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
grep -q "timeout_seconds must be a positive integer" invalid-config.log || {
    cat invalid-config.log; echo "FAIL: invalid hook diagnostic is missing"; exit 1; }

echo "OK"
