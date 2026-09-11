#!/usr/bin/env bash
# requires: pack
# 649_pack_carries_runtime_deploy_files.sh -- `mcpp pack` stages what the build
# placed relative to the executable (`runtime.deploy_files` and
# `runtime.deploy`, #615) at the same relative path beside the packed
# executable. Pack read neither list before, so a program that found its driver
# manifest in the build directory could not find it once packed.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=$HOME/.mcpp

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
"$MCPP" new app > /dev/null
cd app
mkdir -p share/vulkan/icd.d
printf 'icd manifest\n' > share/vulkan/icd.d/lvp_icd.json
printf 'notes\n' > share/notes.txt
cat >> mcpp.toml <<'TOML'

[toolchain]
linux = "gcc@16.1.0"

[runtime]
deploy_files = ["share/notes.txt"]
deploy = [ { from = "share/vulkan/icd.d/lvp_icd.json", to = "vulkan/icd.d" } ]
TOML

"$MCPP" pack > pack.log 2>&1 || fail "mcpp pack failed" pack.log
tarball=$(ls target/dist/app-0.1.0-*.tar.gz 2>/dev/null | head -1)
[ -n "$tarball" ] || fail "no tarball under target/dist" pack.log
mkdir -p "$TMP/x"
tar -xzf "$tarball" -C "$TMP/x"
root=$(ls -d "$TMP"/x/app-0.1.0-*/ | head -1)
[ -x "${root}bin/app" ] || fail "the bundle has no bin/app" pack.log
grep -q 'icd manifest' "${root}bin/vulkan/icd.d/lvp_icd.json" 2>/dev/null \
    || fail "runtime.deploy did not reach bin/vulkan/icd.d in the bundle" pack.log
grep -q 'notes' "${root}bin/notes.txt" 2>/dev/null \
    || fail "runtime.deploy_files did not reach bin/ in the bundle" pack.log
echo "pack carries runtime files OK"
