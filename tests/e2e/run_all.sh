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

# ⚠️ The suite takes ~20 minutes, which is long enough to be tempting to work
# through -- and rebuilding mcpp during it swaps the binary UNDER the run.
# Later tests then measure a different engine from earlier ones, and the report
# mixes the two without saying so. Measured twice in one session: three tests
# "failed", all three passed on a stable binary, and the time went into
# diagnosing regressions that did not exist.
#
# A note asking people not to do it is what failed the second time, so the run
# now records what it started with and says so at the end. It cannot prevent
# the rebuild; it can refuse to let the results be read as if it had not
# happened.
_binary_stamp() {
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$MCPP" | cut -d' ' -f1
    elif command -v sha256sum >/dev/null 2>&1; then sha256sum "$MCPP" | cut -d' ' -f1
    else stat -c '%Y %s' "$MCPP" 2>/dev/null || stat -f '%m %z' "$MCPP" 2>/dev/null
    fi
}
MCPP_STAMP_AT_START="$(_binary_stamp)"

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
        # qemu-riscv: the emulator a bare-metal riscv artifact runs in
        # (xim:qemu-riscv, or any qemu-system-riscv64 on PATH).
        command -v qemu-system-riscv64 &>/dev/null && CAPS+=(qemu-riscv)
        # qemu-arm: the emulator an M-profile artifact runs in. Unlike
        # `qemu-riscv` this also looks inside the xim payload, because
        # `xim:qemu-arm` installs no shim for `qemu-system-arm` on every host
        # and the tests address it by absolute path out of the payload anyway.
        # Probing only PATH would report the capability absent on a machine
        # that has it, and the test would skip while looking supported.
        if command -v qemu-system-arm &>/dev/null \
           || ls "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-qemu-arm/*/bin/qemu-system-arm \
                 &>/dev/null \
           || ls "$HOME"/.xlings/data/xpkgs/xim-x-qemu-arm/*/bin/qemu-system-arm &>/dev/null; then
            CAPS+=(qemu-arm)
        fi
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
        else
            # no-msvc: the bare-Windows shape — a machine with no usable
            # Visual Studio. Declared as its own capability rather than
            # inferred from "not msvc" because a test needs to REQUIRE it:
            # 182 verifies the fallback path, and running it on a box that
            # does have MSVC would exercise the ordinary path and pass while
            # proving nothing. The CI job that masks Visual Studio lands here.
            CAPS+=(no-msvc)
        fi
        # mingw: the WINDOWS-HOSTED MinGW-w64 GCC payload (xim:mingw-gcc,
        # winlibs GCC 16 UCRT). Distinct from `mingw-cross` above, which is the
        # Linux-hosted cross — same target, different host, and a test that needs
        # one cannot use the other.
        #
        # Probing the payload rather than PATH deliberately: a Windows runner may
        # well have some g++.exe from Strawberry Perl, and that one cannot build
        # modules. Same reason the `gcc` capability is withheld here.
        for _mgw in "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-mingw-gcc/*/bin/g++.exe \
                    "$HOME"/.xlings/data/xpkgs/xim-x-mingw-gcc/*/bin/g++.exe; do
            if [[ -x "$_mgw" ]]; then CAPS+=(mingw); break; fi
        done
        unset _mgw
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

# python3: a small number of E2E assertions inspect JSON structurally.  Keep
# this explicit so those tests skip honestly on minimal local runners instead
# of either failing at runtime or declaring an unknown capability.
command -v python3 &>/dev/null && CAPS+=(python3)

# jq: the JSON reader for the tests that consume mcpp's MACHINE interface
# (`--format json`) rather than the tables it prints for people.
#
# ⚠️ DECLARING IT IS PART OF ADDING IT. A `# requires:` token that no branch
# here ever adds makes every test naming it skip for ever, silently — the
# `65_*` block spent months in that state. Every GitHub-hosted runner ships jq,
# so in CI this is always true and a skip there is a red (the matrix workflow
# asserts each invariant's conclusion line); on a minimal local machine the
# skip is honest.
command -v jq &>/dev/null && CAPS+=(jq)

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

# xlings-msvc: mcpp driving the xlings-MANAGED toolset (`msvc@<toolset>`) --
# a different subject from `msvc`, which means "this machine has a Visual
# Studio". Opt-in rather than detected, for two reasons:
#
#   - it downloads ~380 MB (xim:msvc + xim:windows-sdk) and installs a
#     toolchain, so it does not belong in a suite whose other 100+ tests are
#     seconds each;
#   - its subject is the xlings ECOSYSTEM (index -> payload -> unpack ->
#     build -> remove), so it fails for reasons that have nothing to do with
#     the change under test, and mixing it in makes every unrelated PR look
#     broken when the index moves.
#
# It runs in ci-windows-msvc-xlings.yml, which sets this and nothing else.
[[ "${MCPP_E2E_XLINGS_MSVC:-}" == "1" ]] && CAPS+=(xlings-msvc)

echo "Detected capabilities: ${CAPS[*]:-<none>}"

# ---------------------------------------------------------------------------
# Every token a test may declare, across ALL platforms.
# ---------------------------------------------------------------------------
# A test declaring anything else is skipped on every runner, forever, and the
# skip line reads exactly like a legitimate one -- "missing capability: linux"
# is indistinguishable from "missing capability: msvc" on a Linux box. Two
# tests were in that state when this guard was added: 65_toolchain_runtime_
# dirs_for_run.sh (`llvm linux`) had never run in CI at all, and it passes.
#
# This list is the UNIVERSE, not what this machine has: `msvc` is legitimately
# absent on Linux and must stay legal to declare. It is checked against the
# CAPS+=() calls above by tests/e2e/README or by reading them -- keep it in
# sync when adding a capability.
KNOWN_CAPS=(elf fresh-sandbox gcc import-std-libcxx jq llvm macos mingw
            mingw-cross msvc musl nasm no-msvc pack patchelf python3 qemu-riscv
            scan-deps symlink unix-shell windows wine xlings-msvc)

bad_tokens=0
for tf in "$HERE"/[0-9]*.sh; do
    base="$(basename "$tf")"
    req="$(sed -n '2p' "$tf")"
    [[ "$req" =~ ^#\ requires: ]] || continue
    toks="${req#\# requires:}"
    for tok in $toks; do
        if [[ " ${KNOWN_CAPS[*]} " != *" $tok "* ]]; then
            echo "ERROR: $base declares unknown capability '$tok' — it would be"
            echo "       skipped on every runner. Known: ${KNOWN_CAPS[*]}"
            bad_tokens=1
        fi
    done
done
[[ $bad_tokens -eq 0 ]] || exit 1


# ---------------------------------------------------------------------------
# Helper: check if a test's requirements are satisfied
# ---------------------------------------------------------------------------
# Returns 0 (true) if the test should be skipped, prints reason.
# Returns 1 (false) if all requirements are met.

# ⚠️ THERE IS DELIBERATELY NO "requires-hard" FORM.
#
# The obvious answer to "a test silently skipped on the runner that was
# supposed to run it" is a token whose absence FAILS. It was implemented here,
# used once, and measured wrong: a token cannot tell "this runner is
# misconfigured" from "this platform legitimately lacks the capability",
# because the same word means both. `llvm` and `qemu-riscv` are absent on the
# macOS runner by design, so the one test that declared them the hard way made
# the macOS suite fail — a worse outcome than the silent skip it was meant to
# prevent, and one CI reported within the hour.
#
# The guard that works has to know WHICH runner it is talking about, so it
# lives in the job: ci-linux-e2e.yml's `baremetal` job installs the
# capabilities and then asserts the tests' PASS lines actually appeared.
# `run_all.sh` exits 0 on a skip, so its exit code cannot answer that question
# and no token can either.

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

# ---------------------------------------------------------------------------
# Optional sharding:  E2E_SHARD="<index>/<total>"  (1-based)
# ---------------------------------------------------------------------------
# Splits the suite across parallel CI runners. Round-robin over the sorted
# file list rather than contiguous ranges: durations are wildly uneven (the
# timing report below exists precisely because of that), and interleaving
# spreads the long poles instead of stacking them in one shard.
#
# The slice is computed on the FULL file list, before capability filtering,
# so a shard's membership does not depend on what the host can run.
SHARD_IDX=1
SHARD_TOTAL=1
if [[ -n "${E2E_SHARD:-}" ]]; then
    SHARD_IDX="${E2E_SHARD%%/*}"
    SHARD_TOTAL="${E2E_SHARD##*/}"
    if ! [[ "$SHARD_IDX" =~ ^[0-9]+$ && "$SHARD_TOTAL" =~ ^[0-9]+$ ]] \
       || (( SHARD_TOTAL < 1 || SHARD_IDX < 1 || SHARD_IDX > SHARD_TOTAL )); then
        echo "FATAL: bad E2E_SHARD='$E2E_SHARD' (want <index>/<total>, 1-based)"
        exit 1
    fi
    echo "Shard: ${SHARD_IDX}/${SHARD_TOTAL} (round-robin over the suite)"
fi
SHARD_POS=0

# Optional name filter: E2E_ONLY="<glob>" runs just the matching tests.
# Used by the workflows that own ONE subject (see ci-windows-msvc-xlings.yml)
# so the job name and the thing it runs cannot drift apart.
for test in "$HERE"/[0-9]*.sh; do
    if [[ -n "${E2E_ONLY:-}" ]]; then
        # shellcheck disable=SC2053 — glob match is the point
        [[ "$(basename "$test")" == $E2E_ONLY ]] || continue
    fi
    if (( SHARD_TOTAL > 1 )); then
        _mine=$(( (SHARD_POS % SHARD_TOTAL) + 1 ))
        SHARD_POS=$(( SHARD_POS + 1 ))
        (( _mine == SHARD_IDX )) || continue
    fi
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
if [[ "$(_binary_stamp)" != "$MCPP_STAMP_AT_START" ]]; then
    echo
    echo "⚠️  THE BINARY UNDER TEST CHANGED DURING THIS RUN."
    echo "    $MCPP"
    echo "    Earlier tests ran a different engine from later ones, so this"
    echo "    summary describes no single build. Re-run without rebuilding."
    exit 1
fi
if [[ ${#TIMED_OUT_TESTS[@]} -gt 0 ]]; then
    echo "Timed out: ${TIMED_OUT_TESTS[*]}"
fi
if [[ $FAIL -gt 0 ]]; then
    echo "Failed: ${FAILED_TESTS[*]}"
    exit 1
fi
exit 0
