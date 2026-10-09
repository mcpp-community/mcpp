#!/usr/bin/env bash
# requires: gcc llvm
# 899 -- `--toolchain` replacing a toolchain the manifest declared is done as
# asked, and said (D11, design 2026-10-10 §8).
#
#   T1  A different spec: one warning naming both specs, the key and the file.
#   T2  The same spec, or a manifest that declares none: nothing.
#   T3  A member's toolchain from `[workspace.toolchain]` names the workspace
#       root's manifest.
#   T4  It is a warning: `--strict` builds.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

pkg() {  # $1 = dir, $2 = extra manifest text
    mkdir -p "$1/src"
    printf '[package]\nname = "%s"\nversion = "0.1.0"\n%s\n[targets.%s]\nkind = "bin"\nmain = "src/main.cpp"\n' \
        "$(basename "$1")" "$2" "$(basename "$1")" > "$1/mcpp.toml"
    echo 'int main() { return 0; }' > "$1/src/main.cpp"
}

# ── T1, T4 ────────────────────────────────────────────────────────────────
pkg "$TMP/declared" $'[toolchain]\ndefault = "gcc@16.1.0"'
cd "$TMP/declared"
"$MCPP" build --toolchain llvm@23.1.3 --strict > t1.log 2>&1 || fail "--strict failed on the override warning" t1.log
grep -q "warning: --toolchain llvm@23.1.3 replaces gcc@16.1.0 declared at \[toolchain\].default (mcpp.toml)" t1.log \
    || fail "no warning for a replaced toolchain" t1.log
echo "ok T1 T4"

# ── T2 ────────────────────────────────────────────────────────────────────
"$MCPP" build --toolchain gcc@16.1.0 > same.log 2>&1 || fail "same" same.log
if grep -q "replaces" same.log; then fail "warned for the declared spec itself" same.log; fi
pkg "$TMP/undeclared" ""
cd "$TMP/undeclared"
"$MCPP" build --toolchain llvm@23.1.3 > none.log 2>&1 || fail "undeclared" none.log
if grep -q "replaces" none.log; then fail "warned where the manifest declares no toolchain" none.log; fi
echo "ok T2"

# ── T3 ────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/ws" && cd "$TMP/ws"
printf '[workspace]\nmembers = ["app"]\n\n[workspace.toolchain]\ndefault = "gcc@16.1.0"\n' > mcpp.toml
pkg app ""
"$MCPP" build -p app --toolchain llvm@23.1.3 > ws.log 2>&1 || fail "workspace member" ws.log
grep -q "replaces gcc@16.1.0 declared at \[toolchain\].default (mcpp.toml)" ws.log \
    || fail "the warning does not name the workspace root's manifest" ws.log
echo "ok T3"

echo "PASS: 899_toolchain_flag_replacing_a_declared_toolchain_says_so"
