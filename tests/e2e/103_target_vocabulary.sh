#!/usr/bin/env bash
# requires: gcc
# Target-axis vocabulary (naming-unification design §4.2/§4.5/§6.3):
#   1. --target typo → hard error + did-you-mean (never a silent host build)
#   2. planned-tier target → hard error (registered, not yet supported)
#   3. custom triple + explicit [target.X] section → escape hatch proceeds
#   4. [build] target = "<triple>" → default build target without --target
#   5. alias spelling (--target x86_64-unknown-linux-musl) → canonical dir
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new vocab > /dev/null
cd vocab

# ── 1. typo: hard error with did-you-mean ────────────────────────────────────
if "$MCPP" build --target x86_64-linux-mus > "$TMP/typo.log" 2>&1; then
    echo "FAIL: typo'd --target built successfully (silent host fallthrough)"
    cat "$TMP/typo.log"; exit 1
fi
grep -q "unknown target 'x86_64-linux-mus'" "$TMP/typo.log" || {
    echo "FAIL: missing unknown-target error"; cat "$TMP/typo.log"; exit 1; }
grep -q "did you mean 'x86_64-linux-musl'" "$TMP/typo.log" || {
    echo "FAIL: missing did-you-mean suggestion"; cat "$TMP/typo.log"; exit 1; }

# ── 2. planned tier: registered but unsupported → error, not a host build ───
if "$MCPP" build --target riscv64-linux-musl > "$TMP/planned.log" 2>&1; then
    echo "FAIL: planned-tier target built"; cat "$TMP/planned.log"; exit 1
fi
grep -q "not yet supported" "$TMP/planned.log" || {
    echo "FAIL: missing planned-tier error"; cat "$TMP/planned.log"; exit 1; }

# ── 3. escape hatch: custom triple with an explicit [target.X] section ──────
# (pin the host gcc so resolution succeeds; the point is validation lets an
# explicitly-declared custom triple through.)
cat >> mcpp.toml <<'EOF'

[target.myboard-custom-elf]
toolchain = "gcc@16.1.0"
EOF
"$MCPP" build --target myboard-custom-elf > "$TMP/custom.log" 2>&1 || {
    echo "FAIL: explicit [target.X] escape hatch rejected"; cat "$TMP/custom.log"; exit 1; }
grep -q "unknown target" "$TMP/custom.log" && {
    echo "FAIL: escape hatch still errored"; cat "$TMP/custom.log"; exit 1; }

# ── 4. [build] target: project default build target (cargo build.target) ────
git checkout -- mcpp.toml 2>/dev/null || sed -i '/\[target.myboard-custom-elf\]/,+1d' mcpp.toml
if [[ -d "$HOME/.xlings/data/xpkgs/xim-x-musl-gcc" ]] \
   || [[ -d "${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs/xim-x-musl-gcc" ]]; then
    printf '\n[build]\ntarget = "x86_64-linux-musl"\n' >> mcpp.toml
    rm -rf target
    "$MCPP" build > "$TMP/bt.log" 2>&1 || {
        echo "FAIL: [build] target build failed"; cat "$TMP/bt.log"; exit 1; }
    [[ -d target/x86_64-linux-musl ]] || {
        echo "FAIL: [build] target did not select the musl target dir"
        ls target/; exit 1; }

    # ── 5. alias spelling lands in the same canonical directory ─────────────
    "$MCPP" build --target x86_64-unknown-linux-musl > "$TMP/alias.log" 2>&1 || {
        echo "FAIL: alias-spelling build failed"; cat "$TMP/alias.log"; exit 1; }
    [[ -d target/x86_64-unknown-linux-musl ]] && {
        echo "FAIL: alias spelling minted its own target dir"; exit 1; }
else
    echo "(musl toolchain not installed — steps 4-5 skipped)"
fi

echo "OK"
