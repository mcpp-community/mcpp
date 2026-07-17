#!/usr/bin/env bash
# tests/e2e/run_all.sh — run all E2E tests for mcpp
# Usage:  MCPP=/path/to/mcpp ./run_all.sh
#         (or simply ./run_all.sh from repo root after `xmake build`)

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

if [[ -z "${MCPP:-}" ]]; then
    MCPP="$ROOT/build/linux/x86_64/release/mcpp"
fi

if [[ ! -x "$MCPP" ]]; then
    echo "FATAL: mcpp binary not found at $MCPP"
    echo "Run 'xmake build mcpp' first or set MCPP=<path>"
    exit 1
fi

echo "Using mcpp: $MCPP"
$MCPP --version

# mcpp now resolves MCPP_HOME from the binary's location by default.
# In tests we exercise the dev binary at build/.../mcpp, so without an
# explicit override MCPP_HOME would land inside build/ and our cached
# toolchain (sat in ~/.mcpp from prior runs) would be invisible to the
# tests that need it. Pin to ~/.mcpp unless the caller already set it.
# Individual tests that want full isolation override MCPP_HOME again.
if [[ -z "${MCPP_HOME:-}" ]]; then
    export MCPP_HOME="$HOME/.mcpp"
fi
echo "MCPP_HOME: $MCPP_HOME"

# ---------------------------------------------------------------------------
# Capability detection
# ---------------------------------------------------------------------------
# Build the set of capabilities available on this machine/platform.
# Each test declares its needs via a `# requires: cap1 cap2 ...` comment
# on line 2.  Tests with no requirements run everywhere.

CAPS=()
OS="$(uname -s)"

case "$OS" in
    Linux)
        CAPS+=(elf unix-shell fresh-sandbox)
        command -v g++      &>/dev/null && CAPS+=(gcc)
        command -v patchelf &>/dev/null && CAPS+=(patchelf)
        # musl-gcc: check both system PATH and xlings-managed locations
        if command -v x86_64-linux-musl-g++ &>/dev/null \
           || [[ -x "$HOME/.xlings/data/xpkgs/xim-x-musl-gcc/15.1.0/bin/x86_64-linux-musl-g++" ]] \
           || [[ -x "${MCPP_HOME}/registry/data/xpkgs/xim-x-musl-gcc/15.1.0/bin/x86_64-linux-musl-g++" ]]; then
            CAPS+=(musl)
        fi
        # mingw-cross: the Linux-hosted MinGW-w64 cross toolchain (xim
        # mingw-cross-gcc, GCC 16 MSVCRT). Must be the xim-managed GCC-16 build,
        # NOT the distro apt g++-mingw-w64 (GCC 13 — no `import std`). Probe the
        # xlings/mcpp payload location, mirroring the musl probe above.
        if [[ -x "$HOME/.xlings/data/xpkgs/xim-x-mingw-cross-gcc/16.1.0/bin/x86_64-w64-mingw32-g++" ]] \
           || [[ -x "${MCPP_HOME}/registry/data/xpkgs/xim-x-mingw-cross-gcc/16.1.0/bin/x86_64-w64-mingw32-g++" ]]; then
            CAPS+=(mingw-cross)
        fi
        # wine: run cross-built Windows PE artifacts on the Linux host.
        command -v wine &>/dev/null && CAPS+=(wine)
        # pack capability: ELF + patchelf both required
        if [[ " ${CAPS[*]} " == *" patchelf "* ]]; then
            CAPS+=(pack)
        fi
        ;;
    Darwin)
        CAPS+=(unix-shell fresh-sandbox macos)
        # macOS g++ is Apple Clang, not real GCC — don't add gcc capability.
        # Tests requiring gcc need actual GNU GCC (modules, gcm.cache, etc.)
        ;;
    MINGW* | MSYS* | CYGWIN*)
        CAPS+=(windows)
        # Git Bash / MSYS2 on Windows: symlinks need admin or Developer Mode
        if [[ "${MSYS:-}" == *winsymlinks* ]] || cmd.exe /c "mklink /?" &>/dev/null 2>&1; then
            CAPS+=(symlink)
        fi
        # msvc: a system Visual Studio / Build Tools with the VC workload
        # (what `mcpp toolchain default msvc` must be able to detect).
        VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
        if [[ -f "$VSWHERE" ]] \
           && "$VSWHERE" -latest -products '*' \
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
                -property installationPath 2>/dev/null | grep -q .; then
            CAPS+=(msvc)
        fi
        # NOTE: Windows runners may have g++.exe (MinGW/Strawberry) in PATH
        # but it's not a proper mcpp-compatible GCC. Don't add gcc capability.
        # fresh-sandbox: not yet reliable on Windows — xlings LLVM auto-install
        # into temp MCPP_HOME dirs has path/copy issues. Enable once resolved.
        ;;
esac

# symlink: ln -sf works properly on all non-Windows platforms
case "$OS" in
    Linux|Darwin) CAPS+=(symlink) ;;
esac

# nasm: the x86 assembler for .asm sources (PATH — including the xlings
# subos shim — or the mcpp sandbox tool dir).
if command -v nasm &>/dev/null \
   || ls "${MCPP_HOME}/registry/data/xpkgs/xim-x-nasm"/*/nasm 2>/dev/null | head -1 | grep -q . \
   || ls "${MCPP_HOME}/registry/data/xpkgs/xim-x-nasm"/*/bin/nasm 2>/dev/null | head -1 | grep -q .; then
    CAPS+=(nasm)
fi

# scan-deps: clang-scan-deps available (needed for P1689 / Clang dyndep flows)
if command -v clang-scan-deps &>/dev/null \
   || ls "${MCPP_HOME}/registry/data/xpkgs/xim-x-llvm"/*/bin/clang-scan-deps 2>/dev/null | head -1 | grep -q . \
   || ls "${MCPP_HOME}/registry/data/xpkgs/xim-x-llvm"/*/bin/clang-scan-deps.exe 2>/dev/null | head -1 | grep -q .; then
    CAPS+=(scan-deps)
fi

# import-std-libcxx: libc++ std.cppm available (LLVM with libc++ modules)
if ls "${MCPP_HOME}/registry/data/xpkgs/xim-x-llvm"/*/share/libc++/v1/std.cppm 2>/dev/null | head -1 | grep -q .; then
    CAPS+=(import-std-libcxx)
fi

echo "Detected capabilities: ${CAPS[*]:-<none>}"

# ---------------------------------------------------------------------------
# Helper: check if a test's requirements are satisfied
# ---------------------------------------------------------------------------
# Returns 0 (true) if the test should be skipped, prints reason.
# Returns 1 (false) if all requirements are met.

check_requires() {
    local test_file="$1"
    # Read the # requires: line (must be line 2 of the script)
    local req_line
    req_line="$(sed -n '2p' "$test_file")"

    # If there's no requires comment at all, run the test
    [[ "$req_line" =~ ^#\ requires: ]] || return 1

    local caps_needed="${req_line#\# requires:}"
    caps_needed="${caps_needed# }"   # strip leading space

    # Empty requirements → runs everywhere
    [[ -z "$caps_needed" ]] && return 1

    for cap in $caps_needed; do
        if [[ " ${CAPS[*]} " != *" $cap "* ]]; then
            echo "$cap"   # return the missing capability name
            return 0      # should skip
        fi
    done
    return 1  # all satisfied → don't skip
}

# Per-test timeout: bail out of an individual test that gets stuck (e.g.
# 10_env_command.sh has been observed hanging for the full job budget on
# slow xlings/network combinations). 600s default; override via env.
# Linux + git-bash on Windows have GNU `timeout`; macOS may need `gtimeout`
# (coreutils). If neither is present, we run without a wrapper and rely on
# the step-level GitHub Actions timeout-minutes as the backstop.
E2E_TEST_TIMEOUT="${E2E_TEST_TIMEOUT:-600}"
TIMEOUT_CMD=""
if   command -v timeout  &>/dev/null; then TIMEOUT_CMD=timeout
elif command -v gtimeout &>/dev/null; then TIMEOUT_CMD=gtimeout
fi
if [[ -n "$TIMEOUT_CMD" ]]; then
    echo "Per-test timeout: ${E2E_TEST_TIMEOUT}s (via $TIMEOUT_CMD)"
else
    echo "Per-test timeout: <unavailable> (no timeout/gtimeout on PATH)"
fi

# Wall-clock in milliseconds, portable. bash 5 exposes EPOCHREALTIME
# ("secs.usecs"); older bash (e.g. macOS /bin/bash 3.2) falls back to
# whole-second `date`. Used to time each test so slow ones surface for
# later analysis/optimization instead of hiding behind a bare "OK".
_t_ms() {
    if [[ -n "${EPOCHREALTIME:-}" ]]; then
        local er=${EPOCHREALTIME} s us
        s=${er%.*}; us=${er#*.}
        echo $(( 10#$s * 1000 + 10#$us / 1000 ))
    else
        echo $(( $(date +%s) * 1000 ))
    fi
}

# Human-friendly duration from milliseconds: "<Nms" / "1.23s".
_fmt_ms() {
    local ms=$1
    if (( ms < 1000 )); then echo "${ms}ms"; else
        printf '%d.%02ds' $(( ms / 1000 )) $(( (ms % 1000) / 10 ))
    fi
}

PASS=0
FAIL=0
SKIP=0
FAILED_TESTS=()
TIMED_OUT_TESTS=()
TIMINGS=()   # "<ms> <name>" per executed test, for the slowest-first report

for test in "$HERE"/[0-9]*.sh; do
    name="$(basename "$test")"
    echo
    missing_cap="$(check_requires "$test")"
    if [[ -n "$missing_cap" ]]; then
        echo "SKIP: $name (missing capability: $missing_cap)"
        ((SKIP++))
        continue
    fi
    echo "=== $name ==="
    _start_ms=$(_t_ms)
    if [[ -n "$TIMEOUT_CMD" ]]; then
        MCPP="$MCPP" "$TIMEOUT_CMD" "$E2E_TEST_TIMEOUT" bash "$test"
    else
        MCPP="$MCPP" bash "$test"
    fi
    rc=$?
    _dur_ms=$(( $(_t_ms) - _start_ms ))
    TIMINGS+=("$_dur_ms $name")
    _dur="$(_fmt_ms "$_dur_ms")"
    if [[ $rc -eq 0 ]]; then
        echo "PASS: $name (${_dur})"
        ((PASS++))
    elif [[ $rc -eq 124 ]]; then
        # GNU timeout: 124 = killed after deadline (TERM); 137 = SIGKILL after grace.
        echo "TIMEOUT: $name (exceeded ${E2E_TEST_TIMEOUT}s — likely network / xlings stall)"
        ((FAIL++))
        FAILED_TESTS+=("$name (TIMEOUT)")
        TIMED_OUT_TESTS+=("$name")
    else
        echo "FAIL: $name (exit $rc, ${_dur})"
        ((FAIL++))
        FAILED_TESTS+=("$name (exit $rc)")
    fi
done

echo
echo "==============================================="
# Timing report (slowest first) — surfaces the long-pole tests so the suite
# can be sharded/optimized. Also prints the executed-test total wall time.
if [[ ${#TIMINGS[@]} -gt 0 ]]; then
    total_ms=0
    for t in "${TIMINGS[@]}"; do total_ms=$(( total_ms + ${t%% *} )); done
    echo "E2E timing (slowest first; executed total $(_fmt_ms "$total_ms")):"
    printf '%s\n' "${TIMINGS[@]}" | sort -rn | head -15 | while read -r ms nm; do
        printf '  %8s  %s\n' "$(_fmt_ms "$ms")" "$nm"
    done
    echo "==============================================="
fi
echo "E2E Summary: $PASS passed, $FAIL failed, $SKIP skipped"
if [[ ${#TIMED_OUT_TESTS[@]} -gt 0 ]]; then
    echo "Timed out: ${TIMED_OUT_TESTS[*]}"
fi
if [[ $FAIL -gt 0 ]]; then
    echo "Failed: ${FAILED_TESTS[*]}"
    exit 1
fi
exit 0
