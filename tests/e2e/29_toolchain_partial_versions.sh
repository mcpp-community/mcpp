#!/usr/bin/env bash
# 29_toolchain_partial_versions.sh — `mcpp toolchain {install,default}` accept
# partial versions and either positional or @-separated form, AND auto-install
# the default toolchain on a first-run `mcpp build` with no toolchain configured.
#
# We isolate via MCPP_HOME so we don't touch the user's real ~/.mcpp sandbox.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# ─── Section 1: dual-form + partial-version toolchain commands ─────────
export MCPP_HOME="$TMP/h1"

# Pre-install both 15 and 16 with different invocation forms.
"$MCPP" toolchain install gcc 15    > "$TMP/inst1.log" 2>&1 || {
    cat "$TMP/inst1.log"; echo "install 'gcc 15' failed"; exit 1; }
grep -q '15.1.0' "$TMP/inst1.log" || {
    cat "$TMP/inst1.log"; echo "partial '15' didn't resolve to 15.1.0"; exit 1; }

"$MCPP" toolchain install gcc@16   > "$TMP/inst2.log" 2>&1 || {
    cat "$TMP/inst2.log"; echo "install 'gcc@16' failed"; exit 1; }
grep -q '16.1.0' "$TMP/inst2.log" || {
    cat "$TMP/inst2.log"; echo "partial '@16' didn't resolve to 16.1.0"; exit 1; }

# Both versions should appear in `list`.
out=$("$MCPP" toolchain list 2>&1)
[[ "$out" == *"gcc"*"15.1.0"* ]] || { echo "gcc 15.1.0 missing from list:"; echo "$out"; exit 1; }
[[ "$out" == *"gcc"*"16.1.0"* ]] || { echo "gcc 16.1.0 missing from list:"; echo "$out"; exit 1; }

# `default gcc 16` (positional) should pick highest 16.x.y.
"$MCPP" toolchain default gcc 16   > "$TMP/def1.log" 2>&1 || {
    cat "$TMP/def1.log"; echo "default 'gcc 16' failed"; exit 1; }
grep -q 'gcc@16.1.0' "$TMP/def1.log" || {
    cat "$TMP/def1.log"; echo "default 'gcc 16' didn't resolve to 16.1.0"; exit 1; }

# `default gcc@15` (@-form) should switch to 15.1.0.
"$MCPP" toolchain default gcc@15   > "$TMP/def2.log" 2>&1 || {
    cat "$TMP/def2.log"; echo "default 'gcc@15' failed"; exit 1; }
grep -q 'gcc@15.1.0' "$TMP/def2.log" || {
    cat "$TMP/def2.log"; echo "default 'gcc@15' didn't resolve to 15.1.0"; exit 1; }

# ─── Section 2: first-run auto-install ──────────────────────────────────
# Brand-new MCPP_HOME, brand-new package with no [toolchain] declared —
# `mcpp build` should auto-install the canonical default (musl-gcc 15.1
# for portable static binaries) + use it. Output should be a static ELF.
export MCPP_HOME="$TMP/h2"
mkdir -p "$TMP/proj"
cd "$TMP/proj"
"$MCPP" new hello > /dev/null
cd hello

# Sanity: the generated mcpp.toml does not declare a [toolchain] section.
if grep -q '^\[toolchain\]' mcpp.toml; then
    echo "scaffolding regression: mcpp new now adds [toolchain]; update this test"
    exit 1
fi

"$MCPP" build > "$TMP/firstrun.log" 2>&1 || {
    cat "$TMP/firstrun.log"; echo "first-run build failed"; exit 1; }

# Must show the friendly first-run banner AND the build must succeed.
grep -q 'First run' "$TMP/firstrun.log" || {
    cat "$TMP/firstrun.log"; echo "missing First-run banner"; exit 1; }
grep -q 'gcc@15.1.0-musl' "$TMP/firstrun.log" || {
    cat "$TMP/firstrun.log"; echo "first run didn't pick gcc@15.1.0-musl as default"; exit 1; }
grep -q 'Finished' "$TMP/firstrun.log" || {
    cat "$TMP/firstrun.log"; echo "build did not finish"; exit 1; }

# Built binary must exist, run, AND be statically linked (because the
# default toolchain is musl, mcpp infers `linkage = static` automatically).
binary=$(find target -name hello -type f | head -1)
[[ -n "$binary" && -x "$binary" ]] || { echo "no hello binary produced"; exit 1; }
file "$binary" | grep -q 'statically linked' || {
    file "$binary"
    echo "first-run build is not statically linked; musl default not propagated"
    exit 1
}

# Second build should be silent on toolchain — no re-install banner.
"$MCPP" build > "$TMP/secondrun.log" 2>&1 || {
    cat "$TMP/secondrun.log"; echo "second build failed"; exit 1; }
if grep -q 'First run' "$TMP/secondrun.log"; then
    cat "$TMP/secondrun.log"
    echo "second build re-printed First-run banner — default not persisted"
    exit 1
fi

echo "OK"
