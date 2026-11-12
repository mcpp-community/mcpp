#!/usr/bin/env bash
# requires:
# Project pins [toolchain] → mcpp resolves to xpkg absolute path.
#
# This test verifies the resolve_xpkg_path branch is exercised when a
# pinned toolchain spec maps to an existing xpkg payload. We seed the
# sandbox xpkgs from the user's system xlings cache via symlink to
# avoid network-bound install delay. We DO NOT run the full compile
# (that would require glibc/sysroot env activation, which is xlings'
# subos shim concern, not mcpp's toolchain-resolve feature).
set -e
source "$(dirname "$0")/_toolchain_env.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

SANDBOX_HOME="$TMP/mcpp-home"
mkdir -p "$SANDBOX_HOME/registry/data/xpkgs"
# #273: this symlink seed was once the incident vector — post-install fixup
# walked through it and rewrote the REAL ~/.xlings gcc against the throwaway
# sandbox's loader paths. Every rewrite is now fenced by the containment
# predicate (physical location must resolve inside cfg.registryDir), so the
# seed is deliberately KEPT: it doubles as a live canary — if the fence ever
# regresses, the machine's gcc breaks loudly and immediately.
# The payload is the one this machine has: in the home mcpp installs into
# (`mcpp toolchain install gcc <version>`, which is where CI's runners have it)
# or in the system xlings. The probe named the system xlings and 16.1.0 alone
# until 2026-11-12, and the script skipped itself on every runner.
GCC_SEED=""
for store in "$(dirname "$MCPP_E2E_GCC_ROOT")" "$HOME/.xlings/data/xpkgs/xim-x-gcc"; do
    if [[ -d "$store/$GCC_VERSION" ]]; then
        GCC_SEED="$store"
        break
    fi
done
if [[ -n "$GCC_SEED" ]]; then
    ln -s "$GCC_SEED" "$SANDBOX_HOME/registry/data/xpkgs/xim-x-gcc"
else
    echo "SKIP: neither mcpp's home nor the system xlings has gcc $GCC_VERSION"
    echo "OK"
    exit 77
fi

export MCPP_HOME="$SANDBOX_HOME"

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp

cat > mcpp.toml <<EOF
[package]
name        = "myapp"
version     = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[toolchain]
linux = "gcc@${GCC_VERSION}"
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]
[targets.myapp]
kind = "bin"
main = "src/main.cpp"
EOF

# We tolerate compile failure (env-activation issue) — only assert that the
# resolve stage produced the expected output.
out=$("$MCPP" build 2>&1 || true)

# 1. "Resolving" + "toolchain" verb appeared
echo "$out" | grep -q 'Resolving' || { echo "missing Resolving: $out"; exit 1; }
echo "$out" | grep -q 'toolchain'  || { echo "missing toolchain: $out"; exit 1; }

# 2. Resolved line points at the xpkg payload under our sandbox.
#    The display path is now shortened — paths under MCPP_HOME
#    render as `@mcpp/...` — so we match the shortened form
#    rather than the absolute one. The trailing path components are
#    what actually prove the toolchain came from mcpp's sandbox.
echo "$out" | grep -q "Resolved.*gcc@${GCC_VERSION}" || {
    echo "missing Resolved gcc@${GCC_VERSION}: $out"; exit 1; }
echo "$out" | grep -q "@mcpp/registry/data/xpkgs/xim-x-gcc/${GCC_VERSION}/bin/g++" || {
    echo "Resolved path doesn't reference the mcpp sandbox xpkg: $out"; exit 1; }

echo "OK"
