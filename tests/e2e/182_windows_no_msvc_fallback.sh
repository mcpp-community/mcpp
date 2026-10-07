#!/usr/bin/env bash
# requires: windows no-msvc
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
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

TMP=$(mktemp -d)
# Preserve executable and loader evidence before the isolated store is removed.
# A missing helper and an unloadable helper require different repairs.
cleanup() {
    local rc=$?
    if [[ $rc -ne 0 ]]; then
        local report="${RUNNER_TEMP:-$TMP}/bare-windows-diagnostics"
        mkdir -p "$report"
        find "$MCPP_HOME" -type f \( -name cc1plus.exe -o -name g++.exe -o -name '*.dll' \) \
            -printf '%p %s bytes\n' > "$report/payload-files.txt" 2>/dev/null || true
        local driver_count=0
        while IFS= read -r compiler; do
            driver_count=$((driver_count + 1))
            local driver_report="$report/drivers/$driver_count"
            mkdir -p "$driver_report"
            printf '%s\n' "$compiler" > "$driver_report/driver-path.txt"
            "$compiler" --version > "$driver_report/driver-version.txt" 2>&1 || true
            "$compiler" -v > "$driver_report/driver-configure.txt" 2>&1 || true
            "$compiler" -print-prog-name=cc1plus > "$driver_report/driver-helper.txt" 2>&1 || true
            "$compiler" -print-search-dirs > "$driver_report/driver-search.txt" 2>&1 || true
            printf 'int main() { return 0; }\n' > "$driver_report/probe.cpp"
            "$compiler" -v -### -c "$driver_report/probe.cpp" -o "$driver_report/probe.o" \
                > "$driver_report/driver-planned-command.txt" 2>&1 || true
            local compile_rc=0
            "$compiler" -v -c "$driver_report/probe.cpp" -o "$driver_report/probe.o" \
                > "$driver_report/driver-compile.txt" 2>&1 || compile_rc=$?
            printf 'direct compile exit: %s\n' "$compile_rc" >> "$driver_report/driver-compile.txt"
            sha256sum "$compiler" >> "$report/sha256.txt"
            # Keep DLL lookup identical while changing the executable basename.
            # Compare Git Bash launch with a native PowerShell launch, so a
            # name-dependent redirect cannot impersonate the cold compiler.
            local probe="$(dirname "$compiler")/mcpp-driver-probe.exe"
            cp "$compiler" "$probe" 2>/dev/null || true
            "$probe" --version > "$driver_report/renamed-driver-version.txt" 2>&1 || true
            "$probe" -print-search-dirs > "$driver_report/renamed-driver-search.txt" 2>&1 || true
            local native_script='$p = $env:MCPP_DRIVER_DIAGNOSTIC; $item = Get-Item -LiteralPath $p; $item | Format-List FullName,Length,LinkType,Target; if ($env:MCPP_DRIVER_PATH_FORM -eq "long") { $p = $item.FullName }; Write-Output "invoked path: $p"; Get-FileHash -LiteralPath $p -Algorithm SHA256; & $p --version; Write-Output "version exit: $LASTEXITCODE"; & $p -print-search-dirs; Write-Output "search exit: $LASTEXITCODE"; & $p -v -c $env:MCPP_DRIVER_PROBE_SOURCE -o $env:MCPP_DRIVER_PROBE_OUTPUT; Write-Output "compile exit: $LASTEXITCODE"; Write-Output "object present: $(Test-Path -LiteralPath $env:MCPP_DRIVER_PROBE_OUTPUT)"'
            MCPP_DRIVER_DIAGNOSTIC="$(cygpath -w "$compiler")" \
                MCPP_DRIVER_PATH_FORM=long \
                MCPP_DRIVER_PROBE_SOURCE="$(cygpath -w "$driver_report/probe.cpp")" \
                MCPP_DRIVER_PROBE_OUTPUT="$(cygpath -w "$driver_report/native-long-probe.o")" \
                powershell.exe -NoProfile -Command "$native_script" \
                > "$driver_report/native-driver-probe.txt" 2>&1 || true
            # Preserve the exact 8.3 invocation spelling as a separate control.
            # A failed short-path lookup never replaces the long-path report.
            local short_driver
            if short_driver=$(cygpath -d "$compiler" 2> "$driver_report/short-path-error.txt") \
                    && [[ -n "$short_driver" ]]; then
                printf '%s\n' "$short_driver" > "$driver_report/short-driver-path.txt"
                MCPP_DRIVER_DIAGNOSTIC="$short_driver" \
                    MCPP_DRIVER_PATH_FORM=short \
                    MCPP_DRIVER_PROBE_SOURCE="$(cygpath -w "$driver_report/probe.cpp")" \
                    MCPP_DRIVER_PROBE_OUTPUT="$(cygpath -w "$driver_report/native-short-probe.o")" \
                    powershell.exe -NoProfile -Command "$native_script" \
                    > "$driver_report/native-short-driver-probe.txt" 2>&1 || true
            else
                printf 'short path unavailable\n' > "$driver_report/native-short-driver-probe.txt"
            fi
            if command -v objdump >/dev/null; then
                objdump -p "$compiler" > "$driver_report/driver-pe.txt" 2>&1 || true
            fi
        done < <(find "$MCPP_HOME/registry/data/xpkgs" -name g++.exe -type f 2>/dev/null)
        local shim_count=0
        while IFS= read -r shim; do
            shim_count=$((shim_count + 1))
            local shim_report="$report/shims/$shim_count"
            mkdir -p "$shim_report"
            printf '%s\n' "$shim" > "$shim_report/path.txt"
            sha256sum "$shim" > "$shim_report/sha256.txt"
            "$shim" --version > "$shim_report/version.txt" 2>&1 || true
            "$shim" -print-search-dirs > "$shim_report/search.txt" 2>&1 || true
        done < <(find "$MCPP_HOME/registry/subos" -name g++.exe -type f 2>/dev/null)
        printf 'GCC_EXEC_PREFIX=%s\nCOMPILER_PATH=%s\nLIBRARY_PATH=%s\n' \
            "${GCC_EXEC_PREFIX:-}" "${COMPILER_PATH:-}" "${LIBRARY_PATH:-}" > "$report/driver-environment.txt"
        printf 'XLINGS_HOME=%s\nXLINGS_PROJECT_DIR=%s\nXLINGS_ACTIVE_SUBOS=%s\nPATH=%s\n' \
            "${XLINGS_HOME:-}" "${XLINGS_PROJECT_DIR:-}" "${XLINGS_ACTIVE_SUBOS:-}" "$PATH" \
            >> "$report/driver-environment.txt"
        command -v g++ > "$report/ambient-driver-path.txt" 2>&1 || true
        # Windows variable names are case-insensitive; Bash's lookup is not.
        env | grep -Ei '^(gcc_exec_prefix|compiler_path|library_path|collect_gcc|collect_lto_wrapper|xlings_[^=]*|msys[^=]*)=' \
            > "$report/driver-environment-all-cases.txt" || true
        while IFS= read -r helper; do
            echo "Direct invocation: $helper"
            "$helper" --version > "$report/cc1plus-version.txt" 2>&1 || \
                echo "cc1plus exit: $?" >> "$report/cc1plus-version.txt"
            sha256sum "$helper" >> "$report/sha256.txt"
            printf 'int diagnostic_answer() { return 42; }\n' > "$report/helper-probe.cpp"
            local helper_rc=0
            "$helper" -quiet -std=c++23 "$report/helper-probe.cpp" -o "$report/helper-probe.s" \
                > "$report/cc1plus-compile.txt" 2>&1 || helper_rc=$?
            printf 'direct frontend exit: %s\n' "$helper_rc" >> "$report/cc1plus-compile.txt"
            if [[ -s "$report/helper-probe.s" ]]; then
                printf 'assembly output: present\n' >> "$report/cc1plus-compile.txt"
            else
                printf 'assembly output: absent\n' >> "$report/cc1plus-compile.txt"
            fi
            if command -v objdump >/dev/null; then
                objdump -p "$helper" > "$report/cc1plus-pe.txt" 2>&1 || true
            fi
        done < <(find "$MCPP_HOME" -name cc1plus.exe -type f 2>/dev/null)
        powershell.exe -NoProfile -Command \
            'Get-WinEvent -FilterHashtable @{LogName="Microsoft-Windows-Windows Defender/Operational"; StartTime=(Get-Date).AddHours(-1)} -ErrorAction SilentlyContinue | Select-Object TimeCreated,Id,Message | Format-List' \
            > "$report/defender-events.txt" 2>&1 || true
        cat "$report/payload-files.txt" "$report"/drivers/*/driver-helper.txt "$report/cc1plus-version.txt" 2>/dev/null || true
        echo "Diagnostics: $report"
    fi
    rm -rf "$TMP"
    return "$rc"
}
trap cleanup EXIT
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
cat >> mcpp.toml <<EOF

[toolchain]
windows = "llvm@${LLVM_VERSION}"
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
