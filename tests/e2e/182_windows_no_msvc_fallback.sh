#!/usr/bin/env bash
# requires: windows no-msvc
# 182_windows_no_msvc_fallback.sh — a bare Windows box builds with no setup
#
# A stock Windows install has the UCRT runtime DLLs but neither the MSVC STL
# nor the Windows SDK: both arrive only with Visual Studio's "Desktop
# development with C++" workload. mcpp's first-run default used to be clang
# targeting the MSVC ABI, which needs exactly those two — so `mcpp new && mcpp
# build` failed on every such machine, with a diagnostic from clang that said
# nothing about the working alternative sitting right next to it.
#
# The CI job that runs this masks the machine's Visual Studio first. Step 0
# below is what makes that trustworthy: if the masking missed one of
# msvc.cppm's three discovery strategies, mcpp would still find MSVC, take the
# old path, and this test would pass for the wrong reason. Asserting that
# detection FAILS turns that silent false-green into a hard failure.
set -e

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/mcpp-home"      # isolated: no inherited default

# ── 0) Self-check: the environment really has no usable MSVC ────────────────
if "$MCPP" toolchain default msvc >/dev/null 2>&1; then
    echo "FAIL: msvc@system still resolves — the MSVC masking is incomplete,"
    echo "      so nothing below would be testing the no-Visual-Studio path."
    exit 1
fi

# ── 1) First run must just work ─────────────────────────────────────────────
cd "$TMP"
"$MCPP" new bare_win >/dev/null 2>&1 || { echo "FAIL: mcpp new"; exit 1; }
cd bare_win

build_out=$("$MCPP" build 2>&1) || {
    echo "FAIL: first build on a machine without Visual Studio:"
    echo "$build_out"; exit 1; }
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: run: $run_out"; exit 1; }
[[ "$run_out" == *"Hello"* || "$run_out" == *"hello"* ]] \
    || { echo "FAIL: unexpected run output: $run_out"; exit 1; }

# The build must have announced the substitution rather than done it silently.
echo "$build_out" | grep -qi "x86_64-windows-gnu" \
    || { echo "FAIL: fallback not reported in build output:"; echo "$build_out"; exit 1; }

# ── 2) Both axes persisted, so the next invocation is silent and `list`
#       agrees with what the build actually used ───────────────────────────
list_out=$("$MCPP" toolchain list 2>&1)
echo "$list_out" | grep -E '\*\s*gcc' >/dev/null \
    || { echo "FAIL: no gcc starred in Toolchains: $list_out"; exit 1; }
echo "$list_out" | grep -E '\*\s*x86_64-windows-gnu' >/dev/null \
    || { echo "FAIL: x86_64-windows-gnu not starred in Targets: $list_out"; exit 1; }

# A second build must not re-announce the switch — the repair is persisted,
# not re-derived every time.
build2=$("$MCPP" build 2>&1) || { echo "FAIL: second build: $build2"; exit 1; }

# ── 3) The produced exe is self-contained ──────────────────────────────────
EXE=$(find target -name "bare_win.exe" -path "*/bin/*" | head -1)
[[ -n "$EXE" ]] || { echo "FAIL: no exe produced"; exit 1; }
ISO="$TMP/iso"; mkdir -p "$ISO"; cp "$EXE" "$ISO/"
iso_rc=0
iso_out=$(cd "$ISO" && PATH="/usr/bin:/c/Windows/System32" ./bare_win.exe 2>&1) || iso_rc=$?
[[ $iso_rc -eq 0 ]] || {
    echo "FAIL: exe does not run without the toolchain on PATH (rc=$iso_rc): $iso_out"
    exit 1; }

# ── 4) An EXPLICIT choice must not be overruled ────────────────────────────
# mcpp may revise a default it picked itself. It must not silently swap the
# ABI out from under a project that asked for MSVC in writing — a project
# linking vcpkg-built .lib files is far better served by an error.
cd "$TMP"
"$MCPP" new explicit_msvc >/dev/null 2>&1
cd explicit_msvc
cat >> mcpp.toml <<'EOF'

[toolchain]
windows = "llvm@20.1.7"
EOF

set +e
exp_out=$("$MCPP" build 2>&1)
exp_rc=$?
set -e
[[ $exp_rc -ne 0 ]] || {
    echo "FAIL: explicit [toolchain] windows = llvm was silently replaced"
    echo "$exp_out"; exit 1; }
echo "$exp_out" | grep -q "x86_64-windows-gnu" || {
    echo "FAIL: the error does not point at the working alternative:"
    echo "$exp_out"; exit 1; }

echo "PASS: bare Windows — fallback, persistence, self-contained exe, explicit choice respected"
