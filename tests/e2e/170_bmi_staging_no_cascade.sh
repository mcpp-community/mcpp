#!/usr/bin/env bash
# requires:
# 170_bmi_staging_no_cascade.sh — mcpp#311: the std BMI staging edge.
#
# Two invariants, both regressions that shipped:
#
#   1. Re-staging an UNCHANGED std BMI must not recompile anything. The old
#      `cp -f` / `Copy-Item -Force` rule rewrote the file unconditionally and
#      carried no `restat`, so every importer of `import std` rebuilt whenever
#      the cache-side BMI got a newer mtime (which happens on any cwd change
#      before the cache-root fix below).
#   2. The staging SOURCE must live under $MCPP_HOME/bmi. The std module cache
#      used to resolve its root through a private copy of the home logic that
#      knew neither %USERPROFILE% nor self-contained installs, so on Windows it
#      parked the cache in the current working directory as `.mcpp-bmi/`.
#
# Toolchain-neutral: only needs a package that says `import std;`.
set -e

# build.ninja node names are ninja-ESCAPED: on Windows a drive letter arrives as
# `C$:/Users/...`. Unescape before touching the filesystem (the Windows job
# failed on exactly this).
unescape_ninja() { printf '%s' "$1" | sed 's/\$:/:/g; s/\$\$/$/g'; }
# Compare paths across the Windows fork: MCPP_HOME is `C:\Users\...` while ninja
# writes forward slashes.
norm_path() { printf '%s' "$1" | tr '\\' '/'; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
import std;
int main() { std::println("staged"); return 0; }
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
EOF

cd app
"$MCPP" build > build1.log 2>&1 || { cat build1.log; echo "FAIL: initial build"; exit 1; }

NINJA=$(find target -name build.ninja | head -1)
[[ -n "$NINJA" ]] || { echo "FAIL: no build.ninja produced"; exit 1; }

# Parse the staging edge, NOT the rule declaration: `awk '{print $NF}'` over a
# grep for the rule NAME would return the literal rule name.
EDGE=$(grep -E '^build [^ ]+ : stage_file ' "$NINJA" | head -1)
[[ -n "$EDGE" ]] || {
    grep -n "std" "$NINJA" | head -20
    echo "FAIL: no stage_file edge for the std BMI (did import std resolve?)"; exit 1; }
DST=$(unescape_ninja "$(echo "$EDGE" | awk '{print $2}')")
SRC=$(unescape_ninja "$(echo "$EDGE" | awk '{print $NF}')")
echo "staging: $SRC -> $DST"

# ── invariant 2: the cache root is $MCPP_HOME/bmi, never a cwd-local dir ──
HOME_ROOT=$(norm_path "${MCPP_HOME:-$HOME/.mcpp}")
case "$(norm_path "$SRC")" in
    "$HOME_ROOT"/bmi/*) ;;
    *) echo "FAIL: std BMI cache is '$SRC', expected under '$HOME_ROOT/bmi'"; exit 1 ;;
esac
for leftover in "$TMP/.mcpp-bmi" "$TMP/app/.mcpp-bmi"; do
    [[ -d "$leftover" ]] && { echo "FAIL: legacy cache dir created at $leftover"; exit 1; }
done

# ── invariant 1: a dirty staging edge alone must not cascade ──
# mtime only — never mutate the shared cache's CONTENT from a test.
touch "$SRC"
"$MCPP" build -v > build2.log 2>&1 || { cat build2.log; echo "FAIL: rebuild after re-stage"; exit 1; }

grep -qE "stage .*--output" build2.log || {
    cat build2.log; echo "FAIL: the staging edge did not re-run at all"; exit 1; }
if grep -qE '(-c|/c) .*main\.cpp' build2.log; then
    cat build2.log
    echo "FAIL: re-staging an unchanged std BMI recompiled main.cpp (cascade)"
    exit 1
fi

# The staged copy must still be a faithful copy after the no-write path.
# $DST is relative to the build directory (ninja runs with cwd = outputDir).
cmp "$SRC" "$(dirname "$NINJA")/$DST" \
  || { echo "FAIL: staged BMI differs from the cache after staging"; exit 1; }

# And the edge must not stay dirty forever: ninja's restat bookkeeping settles
# it on the next run. (-v so ninja's own line reaches the log — mcpp only
# forwards captured output on failure otherwise.)
"$MCPP" build -v > build3.log 2>&1 || { cat build3.log; echo "FAIL: third build"; exit 1; }
grep -q "no work to do" build3.log || {
    cat build3.log
    echo "FAIL: staging edge stayed dirty after a settled re-stage"; exit 1; }

echo "OK"
