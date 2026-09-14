#!/usr/bin/env bash
# requires: unix-shell
# 674_a_format_is_run_by_the_runner_named_after_it.sh -- `mcpp run --format
# <f>` without `--runner` runs the distributable through the named runner `<f>`
# when one exists, and refuses a directory that no runner reaches before any
# spawn (#634 B3).
#
# Measured on macos-15 against 2026.9.14.1: with a named runner `app` declared,
# `mcpp run --format app --runner app` ran the bundle and returned its status,
# while `mcpp run --format app` alone handed the bundle directory to the kernel:
#
#     ...RApp.app could not be started: Permission denied (error 13)   exit 126
#
# The runner is modelled by scripts that record their operand and then run the
# program, and the bundle by a directory holding a copy of the program, so the
# criteria hold on every POSIX host.
#
# Criteria:
#   1. a runner a build program names after the format (`mcpp::runner("blob",
#      ...)`, as `dist-apple` does for `app`) receives the distributable, the
#      status line says Running, and the program's status comes back;
#   2. a typed `--runner` still wins over the format's name;
#   3. negative direction: a plain `mcpp run` executes the link output without
#      the format's runner;
#   4. a directory distributable that no runner reaches is refused before the
#      spawn, with status 126, naming the runner `bundle` that would reach it;
#   5. a manifest runner named `bundle` then receives the directory;
#   6. negative direction of 4: a default runner reaches the directory, and no
#      refusal is printed.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

for r in blob other bundle default; do
    cat > "$TMP/runner-$r.sh" <<EOF
#!/bin/sh
printf 'RUNNER-$r: %s\n' "\$1"
if [ -d "\$1" ]; then exec "\$1/Contents/MacOS/app"; fi
exec "\$@"
EOF
    chmod +x "$TMP/runner-$r.sh"
done

mkdir -p app/src
cd app
cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("PROGRAM-RAN"); return 7; }
EOF
cat > copy.sh <<'EOF'
#!/usr/bin/env bash
set -e
cp "$1" "$2"
chmod +x "$2"
EOF
cat > bundle.sh <<'EOF'
#!/usr/bin/env bash
set -e
rm -rf "$2"
mkdir -p "$2/Contents/MacOS"
cp "$1" "$2/Contents/MacOS/app"
chmod +x "$2/Contents/MacOS/app"
EOF
chmod +x copy.sh bundle.sh
cat > build.mcpp <<EOF
import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("blob");
    mcpp::provides_pack_format("bundle");
    mcpp::runner("blob", "$TMP/runner-blob.sh");
    const std::string_view fmt = mcpp::pack_format();
    if (fmt != "blob" && fmt != "bundle") return 0;
    const std::string root = mcpp::manifest_dir();
    const bool dir = fmt == "bundle";
    const std::string out = std::string(mcpp::out_dir()) + (dir ? "/app.bundle" : "/app.blob");
    mcpp::action a;
    a.id          = dir ? "bundle" : "blob";
    a.role        = "artifact";
    a.description = a.id;
    a.arg((root + (dir ? "/bundle.sh" : "/copy.sh")).c_str())
     .arg("\${mcpp.target_file:app}")
     .arg(out.c_str())
     .input("\${mcpp.target_file:app}")
     .output(out.c_str())
     .submit();
    return 0;
}
EOF

set +e
"$MCPP" build > b0.log 2>&1 || fail "the initial build failed" b0.log
set -e
HOST=$(ls target | head -1)
[ -n "$HOST" ] || fail "could not determine the host triple from target/" b0.log
printf '\n[target.%s.runners]\nother = ["%s"]\n' "$HOST" "$TMP/runner-other.sh" >> mcpp.toml
cp mcpp.toml mcpp.toml.base

# ── 1. the runner named after the format ──────────────────────────────────
set +e
"$MCPP" run --format blob > r1.log 2>&1; rc=$?
set -e
grep -q "RUNNER-blob: .*/app\.blob$" r1.log \
    || fail "the runner named 'blob' did not receive the distributable" r1.log
grep -q "PROGRAM-RAN" r1.log || fail "the program did not run through the runner" r1.log
[ "$rc" -eq 7 ] || fail "the program's status 7 did not come back (got $rc)" r1.log
grep -q "Running \`.*runner-blob.sh" r1.log \
    || fail "the status line does not say Running for the format's runner" r1.log
echo "mcpp run --format blob runs through the runner named blob OK"

# ── 2. a typed --runner wins ───────────────────────────────────────────────
set +e
"$MCPP" run --format blob --runner other > r2.log 2>&1; rc=$?
set -e
grep -q "RUNNER-other: .*/app\.blob$" r2.log || fail "--runner other did not win" r2.log
grep -q "RUNNER-blob" r2.log && fail "the format's runner ran although --runner was typed" r2.log
[ "$rc" -eq 7 ] || fail "--runner other lost the program's status (got $rc)" r2.log
echo "a typed --runner wins over the format's name OK"

# ── 3. a plain run is unaffected ───────────────────────────────────────────
set +e
"$MCPP" run > r3.log 2>&1; rc=$?
set -e
grep -q "RUNNER-" r3.log && fail "a plain run went through a runner" r3.log
grep -q "PROGRAM-RAN" r3.log || fail "a plain run did not run the program" r3.log
[ "$rc" -eq 7 ] || fail "a plain run lost the program's status (got $rc)" r3.log
echo "a plain mcpp run executes the link output directly OK"

# ── 4. a directory no runner reaches ───────────────────────────────────────
set +e
"$MCPP" run --format bundle > r4.log 2>&1; rc=$?
set -e
[ "$rc" -eq 126 ] || fail "the directory without a runner did not exit 126 (got $rc)" r4.log
grep -q "produced a directory" r4.log || fail "the refusal does not say the distributable is a directory" r4.log
grep -q "a runner named 'bundle'" r4.log || fail "the refusal does not name the runner 'bundle'" r4.log
grep -q "\[target\.$HOST\.runners\]" r4.log || fail "the refusal does not name the manifest table" r4.log
grep -q "Permission denied\|Is a directory" r4.log && fail "the directory reached the kernel" r4.log
grep -q "PROGRAM-RAN\|RUNNER-" r4.log && fail "something ran despite the refusal" r4.log
echo "a directory no runner reaches is refused before the spawn OK"

# ── 5. a manifest runner named after the format reaches the directory ─────
printf 'bundle = ["%s"]\n' "$TMP/runner-bundle.sh" >> mcpp.toml
set +e
"$MCPP" run --format bundle > r5.log 2>&1; rc=$?
set -e
grep -q "RUNNER-bundle: .*/app\.bundle$" r5.log || fail "the runner named 'bundle' did not receive the directory" r5.log
[ "$rc" -eq 7 ] || fail "the bundle's program status did not come back (got $rc)" r5.log
echo "a manifest runner named bundle runs the directory OK"

# ── 6. a default runner reaches the directory without a refusal ───────────
cp mcpp.toml.base mcpp.toml
printf '\n[target.%s]\nrunner = ["%s"]\n' "$HOST" "$TMP/runner-default.sh" >> mcpp.toml
set +e
"$MCPP" run --format bundle > r6.log 2>&1; rc=$?
set -e
grep -q "RUNNER-default: .*/app\.bundle$" r6.log || fail "the default runner did not receive the directory" r6.log
grep -q "no runner reaches it" r6.log && fail "a refusal was printed although a default runner exists" r6.log
[ "$rc" -eq 7 ] || fail "the default runner lost the status (got $rc)" r6.log
echo "a default runner reaches the directory, no refusal OK"

echo "PASS: 674_a_format_is_run_by_the_runner_named_after_it"
