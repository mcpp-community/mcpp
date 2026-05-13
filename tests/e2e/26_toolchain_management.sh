#!/usr/bin/env bash
# 26_toolchain_management.sh — verify M5.5 toolchain CLI + isolation:
#   - mcpp toolchain install / list / default / remove
#   - hard error when no toolchain configured
#   - global default lets project-level [toolchain] be omitted
#   - mcpp uses ~/.mcpp/registry/, never falls back to system PATH
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
# Disable first-run auto-install — we explicitly drive install/list/default
# through the CLI here, and want any silent auto-install to surface as a
# test failure rather than a successful build under a different default.
export MCPP_NO_AUTO_INSTALL=1

# 1) toolchain list with empty home
out=$("$MCPP" toolchain list 2>&1)
[[ "$out" == *"no toolchains installed"* ]] || { echo "FAIL: empty list: $out"; exit 1; }

# 2) hard error when no toolchain configured (with MCPP_NO_AUTO_INSTALL).
mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
cat > mcpp.toml <<'EOF'
[package]
name    = "tinybin"
version = "0.1.0"
EOF
cat > src/main.cpp <<'EOF'
import std;
int main() { std::println("ok"); return 0; }
EOF
rc=0
out=$("$MCPP" build 2>&1) || rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL: build should hard-error without toolchain"; exit 1; }
[[ "$out" == *"no toolchain configured"* ]] || { echo "FAIL: error not helpful: $out"; exit 1; }

# 3) Install gcc 16.1.0
if [[ -n "${MCPP_E2E_TOOLCHAIN_MIRROR:-}" ]]; then
    "$MCPP" self config --mirror "$MCPP_E2E_TOOLCHAIN_MIRROR"
    "$MCPP" self config
fi
rc=0
"$MCPP" toolchain install gcc 16.1.0 > /tmp/_inst.log 2>&1 || rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: install exited $rc; log:"
    cat /tmp/_inst.log
    echo ""
    echo "--- diagnostic: sandbox tree depth 4 ---"
    find "$MCPP_HOME/registry" -maxdepth 4 -type d 2>&1 | head -20
    echo "--- xpkgs contents ---"
    ls "$MCPP_HOME/registry/data/xpkgs/" 2>&1 | head -10
    echo "--- xim-x-gcc tree ---"
    find "$MCPP_HOME/registry/data/xpkgs/xim-x-gcc" 2>&1 | head -20
    echo "--- any g++ ---"
    find "$MCPP_HOME" -name g++ 2>&1 | head -5
    echo "--- runtimedir gcc tarball ---"
    ls "$MCPP_HOME/registry/data/runtimedir/" 2>&1 | head
    exit 1
fi
grep -q 'Installed' /tmp/_inst.log || { cat /tmp/_inst.log; echo "FAIL: no Installed line"; exit 1; }

# 4) toolchain list now non-empty
out=$("$MCPP" toolchain list 2>&1)
echo "$out" | grep -q 'gcc' || { echo "FAIL: list missing gcc: $out"; exit 1; }
echo "$out" | grep -q '16.1.0' || { echo "FAIL: list missing version: $out"; exit 1; }

# 5) Set as default
"$MCPP" toolchain default gcc@16.1.0 > /tmp/_def.log 2>&1
grep -q 'Default' /tmp/_def.log || { cat /tmp/_def.log; echo "FAIL: no Default msg"; exit 1; }
out=$("$MCPP" toolchain list 2>&1)
echo "$out" | grep -q '\*' || { echo "FAIL: default mark missing: $out"; exit 1; }

# 6) Build now succeeds (project has no [toolchain]; uses global default)
"$MCPP" build > /tmp/_build.log 2>&1 || { cat /tmp/_build.log; echo "FAIL: build failed"; exit 1; }
grep -q 'Finished' /tmp/_build.log || { cat /tmp/_build.log; echo "FAIL: no Finished"; exit 1; }
triple=$(ls -d target/x86_64-linux-*/ | head -1)
fp_dir=$(ls "$triple")
[[ "$(${triple}${fp_dir}/bin/tinybin)" == "ok" ]] || { echo "FAIL: runtime"; exit 1; }

# 7) The compile commands in build.ninja should reference the mcpp-private
#    binary, NOT the system xlings wrapper.
grep -q "$MCPP_HOME/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++" \
    "$triple$fp_dir/build.ninja" || {
    echo "FAIL: build.ninja doesn't reference private binary"
    grep -m1 '^cxx' "$triple$fp_dir/build.ninja"
    exit 1
}
if grep -q '/usr/bin/g++\|/usr/local/bin/g++' "$triple$fp_dir/build.ninja"; then
    echo "FAIL: build.ninja referenced system path"; exit 1
fi

# 8) `mcpp env` reports the default
out=$("$MCPP" self env 2>&1)
echo "$out" | grep -q 'default toolchain   = gcc@16.1.0' || {
    echo "FAIL: env missing default toolchain line"; exit 1; }

# 9) Remove
rm -rf target
"$MCPP" toolchain remove gcc@16.1.0 > /tmp/_rm.log 2>&1
grep -q 'Removed' /tmp/_rm.log || { cat /tmp/_rm.log; echo "FAIL: no Removed msg"; exit 1; }

# 10) After removing, the toolchain dir is gone, but config.toml still
#     points to it as default. Verify list shows nothing for that compiler now.
out=$("$MCPP" toolchain list 2>&1)
if echo "$out" | grep -qE '^gcc.*15\.1\.0'; then
    echo "FAIL: list still shows removed toolchain: $out"; exit 1
fi

echo "OK"
