#!/usr/bin/env bash
# requires: gcc
# 745 -- `mcpp test --no-run` builds the tests and reports that, and the
# report is distinguishable from the one mcpp gives when it tried to run them
# and could not.
#
# WHY THE TWO HAVE TO DIFFER. A target this host cannot execute leaves every
# test `not run` and the command exits 2, which is the correct answer to "do
# these tests pass": mcpp did not find out. But 2 is also what a missing or
# broken runner returns, so a caller that wanted only the build could not tell
# the two apart and had to use `mcpp build` instead -- which builds the
# PACKAGE, and for a package whose only sources are under `tests/` compiles
# nothing of it whatsoever. Measured on mcpp-index's `archive` member, whose
# sources are two files under `tests/`: `mcpp build --target aarch64-macos`
# exits 0 having compiled the member's dependencies and not one line of the
# member, and a compatibility sweep reading that exit code records the member
# as building on macOS.
#
# THE RUNNER HERE IS A NAME THAT IS NOT A PROGRAM, AND THAT IS THE POINT.
# Using an unexecutable target would make this test need a cross toolchain and
# a host that cannot run it; a declared runner that does not exist produces the
# same situation -- tests built, nothing run -- on every host, for the native
# target, with nothing installed.
#
# Five legs:
#   A  without `--no-run`: exit 2, and the tests are reported `not run`.
#   B  with `--no-run`: exit 0, and the count is reported as built.
#   C  a test that does not COMPILE is still a failure under `--no-run`.
#      Without C, an implementation that reported everything as built whether
#      or not it built would pass A and B.
#   D  `--no-run` with `--no-runner` is refused. The two names differ by one
#      character and mean opposite things, and neither is a weaker form of the
#      other, so there is no reading of the pair to prefer.
#   E  `--workspace` totals the built count. A count that stops at the member
#      level leaves the same false reading one level up.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

# NO `src/`: the package's only sources are its tests, which is the shape the
# motivating case has and the shape `mcpp build` compiles nothing of.
mkdir -p tests
cat > mcpp.toml <<'EOF'
[package]
name    = "norun"
version = "0.1.0"

# A name, not a path, and not a program: the runner lookup fails, which is the
# situation this test is about.
[target.HOST_TRIPLE]
runner = ["mcpp-no-such-runner-exists"]
EOF
cat > tests/alpha.cpp <<'EOF'
int main() { return 0; }
EOF
cat > tests/beta.cpp <<'EOF'
int main() { return 0; }
EOF

host="$("$MCPP" --print-target 2>/dev/null || true)"
if [ -z "$host" ]; then
    # `--print-target` postdates some clients; take the triple from a build
    # directory instead, which every version writes.
    "$MCPP" build >/dev/null 2>&1 || true
    host="$(ls target 2>/dev/null | grep -v '^\.' | head -1)"
fi
[ -n "$host" ] || { echo "FAIL: could not determine the host triple"; exit 1; }
sed -i.bak "s/HOST_TRIPLE/$host/" mcpp.toml && rm -f mcpp.toml.bak

# --- A: it tried to run them and could not -------------------------------
set +e
out_a="$("$MCPP" test --target "$host" 2>&1)"
rc_a=$?
set -e
printf '%s\n' "$out_a" | tail -3
if [ "$rc_a" != 2 ]; then
    echo "FAIL: A expected exit 2 from a runner that does not exist, got $rc_a"
    exit 1
fi
case "$out_a" in
    *"not run"*) ;;
    *) echo "FAIL: A did not report the tests as not run"; exit 1 ;;
esac

# --- B: it was asked not to ----------------------------------------------
set +e
out_b="$("$MCPP" test --target "$host" --no-run 2>&1)"
rc_b=$?
set -e
printf '%s\n' "$out_b" | tail -3
if [ "$rc_b" != 0 ]; then
    echo "FAIL: B expected exit 0 under --no-run, got $rc_b"
    exit 1
fi
case "$out_b" in
    *"2 built, not run"*) ;;
    *) echo "FAIL: B did not report two tests as built"; exit 1 ;;
esac
# The word that distinguishes B from A must not appear in B: "N not run" is
# the reading B exists to replace, and a summary carrying both says neither.
case "$out_b" in
    *"2 not run"*) echo "FAIL: B reported the tests as not run as well as built"; exit 1 ;;
esac

# --- C: --no-run does not make a broken test pass -------------------------
cat > tests/gamma.cpp <<'EOF'
int main() { this_function_does_not_exist(); }
EOF
set +e
out_c="$("$MCPP" test --target "$host" --no-run 2>&1)"
rc_c=$?
set -e
if [ "$rc_c" = 0 ]; then
    echo "FAIL: C a test that does not compile was reported as built"
    printf '%s\n' "$out_c" | tail -5
    exit 1
fi
# Non-zero is not enough: it must be non-zero BECAUSE `gamma` did not build,
# and the other two must still be reported as built.
case "$out_c" in
    *gamma*) ;;
    *) echo "FAIL: C failed without naming the test that did not compile"; exit 1 ;;
esac
case "$out_c" in
    *"2 built, not run"*) ;;
    *) echo "FAIL: C stopped reporting the tests that did build"; printf '%s\n' "$out_c" | tail -4; exit 1 ;;
esac

# --- D: the two spellings are not a preference ----------------------------
set +e
out_d="$("$MCPP" test --target "$host" --no-run --no-runner 2>&1)"
rc_d=$?
set -e
if [ "$rc_d" = 0 ]; then
    echo "FAIL: D --no-run with --no-runner was accepted"
    exit 1
fi
case "$out_d" in
    *"cannot be combined"*) ;;
    *) echo "FAIL: D did not explain why the pair is refused"; printf '%s\n' "$out_d" | tail -3; exit 1 ;;
esac

# --- E: the workspace total says it too ----------------------------------
# A count that stops at the member level is the same defect one level up: a
# workspace summary reading "0 passed; 0 failed" is what a workspace with no
# tests reports.
rm -f tests/gamma.cpp
mkdir -p members/one/tests members/two/tests
cat > mcpp.toml <<EOF
[workspace]
members = ["members/one", "members/two"]
EOF
for m in one two; do
    cat > "members/$m/mcpp.toml" <<EOF
[package]
name    = "$m"
version = "0.1.0"

[target.$host]
runner = ["mcpp-no-such-runner-exists"]
EOF
    cat > "members/$m/tests/t.cpp" <<'EOF'
int main() { return 0; }
EOF
done
set +e
out_e="$("$MCPP" test --workspace --target "$host" --no-run 2>&1)"
rc_e=$?
set -e
printf '%s
' "$out_e" | tail -3
if [ "$rc_e" != 0 ]; then
    echo "FAIL: E expected exit 0 from a --workspace --no-run run, got $rc_e"
    exit 1
fi
case "$out_e" in
    *"2 built, not run"*) ;;
    *) echo "FAIL: E the workspace total did not report the built tests"; exit 1 ;;
esac

echo "PASS: --no-run builds the tests and says so, and says nothing else"
