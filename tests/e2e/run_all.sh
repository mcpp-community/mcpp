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
        CAPS+=(elf unix-shell)
        command -v g++      &>/dev/null && CAPS+=(gcc)
        command -v patchelf &>/dev/null && CAPS+=(patchelf)
        # musl-gcc: check both system PATH and xlings-managed locations
        if command -v x86_64-linux-musl-g++ &>/dev/null \
           || [[ -x "$HOME/.xlings/data/xpkgs/xim-x-musl-gcc/15.1.0/bin/x86_64-linux-musl-g++" ]] \
           || [[ -x "${MCPP_HOME}/registry/data/xpkgs/xim-x-musl-gcc/15.1.0/bin/x86_64-linux-musl-g++" ]]; then
            CAPS+=(musl)
        fi
        # pack capability: ELF + patchelf both required
        if [[ " ${CAPS[*]} " == *" patchelf "* ]]; then
            CAPS+=(pack)
        fi
        ;;
    Darwin)
        CAPS+=(unix-shell)
        command -v g++      &>/dev/null && CAPS+=(gcc)
        ;;
    MINGW* | MSYS* | CYGWIN*)
        # Git Bash / MSYS2 on Windows: symlinks need admin or Developer Mode
        if [[ "${MSYS:-}" == *winsymlinks* ]] || cmd.exe /c "mklink /?" &>/dev/null 2>&1; then
            CAPS+=(symlink)
        fi
        # NOTE: Windows runners may have g++.exe (MinGW/Strawberry) in PATH
        # but it's not a proper mcpp-compatible GCC. Don't add gcc capability.
        ;;
esac

# symlink: ln -sf works properly on all non-Windows platforms
case "$OS" in
    Linux|Darwin) CAPS+=(symlink) ;;
esac

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

PASS=0
FAIL=0
SKIP=0
FAILED_TESTS=()

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
    if MCPP="$MCPP" bash "$test"; then
        echo "PASS: $name"
        ((PASS++))
    else
        echo "FAIL: $name"
        ((FAIL++))
        FAILED_TESTS+=("$name")
    fi
done

echo
echo "==============================================="
echo "E2E Summary: $PASS passed, $FAIL failed, $SKIP skipped"
if [[ $FAIL -gt 0 ]]; then
    echo "Failed: ${FAILED_TESTS[@]}"
    exit 1
fi
exit 0
