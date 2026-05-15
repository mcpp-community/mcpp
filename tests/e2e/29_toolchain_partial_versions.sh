#!/usr/bin/env bash
# 29_toolchain_partial_versions.sh — `mcpp toolchain default` accepts partial
# versions in either positional or @-separated form, AND `mcpp build`
# auto-installs the default toolchain on a first run with no toolchain
# configured. The full install path is covered by 26_toolchain_management.sh.
#
# We isolate config/default state via MCPP_HOME, while reusing already prepared
# xlings payloads when available so CI does not redownload full toolchains.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

inherit_payloads_only() {
    MCPP_INHERIT_CONFIG=0 MCPP_INHERIT_SUBOS=0 source "$(dirname "$0")/_inherit_toolchain.sh"
}

configure_e2e_mirror() {
    if [[ -n "${MCPP_E2E_TOOLCHAIN_MIRROR:-}" ]]; then
        "$MCPP" self config --mirror "$MCPP_E2E_TOOLCHAIN_MIRROR" > "$TMP/mirror.log" 2>&1 || {
            cat "$TMP/mirror.log"
            echo "failed to configure e2e mirror"
            exit 1
        }
    fi
}

# ─── Section 1: dual-form + partial-version toolchain commands ─────────
export MCPP_HOME="$TMP/h1"
inherit_payloads_only
configure_e2e_mirror

# Reuse the CI-prepared gcc payload. The full install path is covered by
# 26_toolchain_management.sh; this test focuses on partial/default parsing
# without redownloading large toolchain archives.
out=$("$MCPP" toolchain list 2>&1)
[[ "$out" == *"gcc"*"16.1.0"* ]] || { echo "gcc 16.1.0 missing from list:"; echo "$out"; exit 1; }

# `default gcc 16` (positional) should pick highest 16.x.y.
"$MCPP" toolchain default gcc 16   > "$TMP/def1.log" 2>&1 || {
    cat "$TMP/def1.log"; echo "default 'gcc 16' failed"; exit 1; }
grep -q 'gcc@16.1.0' "$TMP/def1.log" || {
    cat "$TMP/def1.log"; echo "default 'gcc 16' didn't resolve to 16.1.0"; exit 1; }

# `default gcc@16` (@-form) should also resolve to 16.1.0.
"$MCPP" toolchain default gcc@16   > "$TMP/def2.log" 2>&1 || {
    cat "$TMP/def2.log"; echo "default 'gcc@16' failed"; exit 1; }
grep -q 'gcc@16.1.0' "$TMP/def2.log" || {
    cat "$TMP/def2.log"; echo "default 'gcc@16' didn't resolve to 16.1.0"; exit 1; }

# ─── Section 2: first-run auto-install ──────────────────────────────────
# Brand-new MCPP_HOME, brand-new package with no [toolchain] declared —
# `mcpp build` should auto-install the canonical default (musl-gcc 15.1
# for portable static binaries) + use it. Output should be a static ELF.
export MCPP_HOME="$TMP/h2"
configure_e2e_mirror
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
