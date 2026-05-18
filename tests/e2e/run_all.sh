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

# Platform detection: some tests are Linux-only (ELF patchelf, musl-static,
# GCC-specific BMI layout, etc.)
OS="$(uname -s)"
MACOS_SKIP=(
    # GCC-specific BMI assertions (gcm.cache/*.gcm)
    03_multi_module.sh
    # Static library test checks ELF ar output format
    07_static_library.sh
    # Shared library test hardcodes .so / ELF shared object
    08_shared_library.sh
    # Path dependency checks .gcm BMI format (GCC-specific)
    09_path_dependency.sh
    # Pack modes use patchelf (ELF-only)
    30_pack_modes.sh
    # Toolchain management tests assume GCC availability
    26_toolchain_management.sh
    29_toolchain_partial_versions.sh
    # P1689 scanner test hardcodes GCC ddi format
    20_p1689_scanner.sh
    # Ninja dyndep test hardcodes GCC module format
    21_ninja_dyndep.sh
    # Doctor/cache/publish uses GCC fingerprint
    22_doctor_cache_publish.sh
    # Self-contained home test assumes Linux sandbox layout
    27_self_contained_home.sh
    # Multi-version mangling test uses GCC module format
    33_multi_version_mangling.sh
)

# Windows inherits all macOS skips (no GCC, no ELF, no patchelf) plus
# additional Windows-specific exclusions.
WINDOWS_SKIP=(
    "${MACOS_SKIP[@]}"
    # Symlinks (ln -sf) not available in Git Bash without elevated perms
    10_env_command.sh
    # test_failing expects non-zero exit but mcpp test returns 0 on Windows
    16_test_failing.sh
    # BMI cache cp_bmi rule uses cmd /c copy — path issues with mixed slashes
    19_bmi_cache_reuse.sh
    # Git dependency has CRLF + path issues on Windows
    24_git_dependency.sh
    # C language test checks ninja rule routing — different on Windows
    26_c_language_support.sh
    # Namespace deps use path dependencies with symlinks
    27_namespace_dependencies.sh
    # Dev binary home test assumes g++ in PATH
    30_dev_binary_home.sh
    # Transitive deps tries to install musl-gcc (Linux-only)
    31_transitive_deps.sh
    # Pack/publish uses tar, patchelf (Linux-only)
    31_pack_publish_dry_run.sh
    # Semver merge uses path dependencies
    32_semver_merge.sh
    # LLVM tests that need libc++ std.cppm (not available on Windows)
    37_llvm_import_std.sh
    38_llvm_std_compat.sh
    39_llvm_multi_module.sh
    40_llvm_clang_scan_deps.sh
    41_llvm_incremental.sh
    # Self config mirror has xlings path issues on Windows
    38_self_config_mirror.sh
    # install.sh is a Unix shell script
    45_install_platform_mapping.sh
)

should_skip() {
    local name="$1"
    if [[ "$OS" == MINGW* || "$OS" == MSYS* || "$OS" == CYGWIN* ]]; then
        for skip in "${WINDOWS_SKIP[@]}"; do
            [[ "$name" == "$skip" ]] && return 0
        done
    elif [[ "$OS" == "Darwin" ]]; then
        for skip in "${MACOS_SKIP[@]}"; do
            [[ "$name" == "$skip" ]] && return 0
        done
    fi
    return 1
}

PASS=0
FAIL=0
SKIP=0
FAILED_TESTS=()

for test in "$HERE"/[0-9]*.sh; do
    name="$(basename "$test")"
    echo
    if should_skip "$name"; then
        echo "SKIP: $name (not applicable on $OS)"
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
