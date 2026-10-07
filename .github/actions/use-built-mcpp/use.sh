#!/usr/bin/env bash
# The body of the use-built-mcpp action; see action.yml for what it provides.
#
# Usage: use.sh <host> <mirror>
set -euo pipefail

host="$1"
mirror="$2"

dir="$RUNNER_TEMP/mcpp-built"
case "$host" in
    windows-*) exe=mcpp.exe; dir="$(cygpath -u "$dir")" ;;
    *)         exe=mcpp ;;
esac
bin="$dir/$exe"
if [ ! -f "$bin" ]; then
    echo "::error::the artifact mcpp-built-$host holds no $exe"
    ls -la "$dir" || true
    exit 1
fi
chmod +x "$bin"

boot="${MCPP:-}"
if [ -z "$boot" ]; then
    echo "::error::MCPP is unset: run bootstrap-mcpp or setup-macos-llvm before use-built-mcpp"
    exit 1
fi

# The toolchain mcpp.toml names for this host is the one the build used, so it
# is the one whose payloads hold the binary's runtime.
manifest_toolchain() {
    # Native ARM64 overrides the platform-wide GCC pin. Consumers must install
    # the same payload used by build.yml, including the first launch's runtime.
    if [ "$host" = linux-aarch64 ]; then
        local native
        native="$(awk '
            /^\[/ { in_tc = ($0 == "[target.aarch64-linux-gnu]"); next }
            in_tc && $1 == "toolchain" { gsub(/"/, "", $3); print $3; exit }
        ' mcpp.toml)"
        if [ -n "$native" ]; then
            printf '%s\n' "$native"
            return
        fi
    fi
    local key
    case "$host" in
        macos-*)   key=macos ;;
        windows-*) key=windows ;;
        *)         key=default ;;
    esac
    awk -v k="$key" '
        /^\[/ { in_tc = ($0 == "[toolchain]") ; next }
        in_tc && $1 == k { gsub(/"/, "", $3); print $3; exit }
    ' mcpp.toml
}

if ! out=$("$bin" --version 2>&1); then
    tc="$(manifest_toolchain)"
    echo "this commit's mcpp does not run yet ($out); installing ${tc:-the default toolchain} with the bootstrap"
    if [ -n "$tc" ]; then
        "$boot" toolchain install "${tc%@*}" "${tc#*@}"
    fi
    if ! out=$("$bin" --version 2>&1); then
        echo "::error::this commit's mcpp does not run on this runner: $out"
        exit 1
    fi
fi
echo "this commit's mcpp: $out ($bin)"

# The mirror first: the runners are outside CN, and the install below and
# every later download read it.
if [ -n "${XLINGS_BIN:-}" ]; then
    "$XLINGS_BIN" config --mirror "$mirror" 2>/dev/null || true
fi
MCPP_VENDORED_XLINGS="${XLINGS_BIN:-}" "$bin" self config --mirror "$mirror"

# THE TOOLCHAIN THE BUILD USED IS INSTALLED, AS IT WAS WHEN EVERY JOB BUILT.
# A job that built mcpp itself installed this toolchain as a side effect, and
# the steps after the build relied on it without saying so: measured on the
# first run of this action, the aarch64 leg of the target matrix restored no
# sandbox, its binary ran without any payload, and the invariants that list the
# host's toolchains found none ("gcc is not installed here"). Installing it here
# keeps every consumer's environment what it was. It is a lookup when the
# toolchain is present.
tc="$(manifest_toolchain)"
if [ -n "$tc" ]; then
    MCPP_VENDORED_XLINGS="${XLINGS_BIN:-}" "$bin" toolchain install "${tc%@*}" "${tc#*@}"
fi

{
    echo "MCPP_BOOT=$boot"
    echo "MCPP=$bin"
    echo "MCPP_FRESH=$bin"
    if [ -n "${XLINGS_BIN:-}" ]; then echo "MCPP_VENDORED_XLINGS=$XLINGS_BIN"; fi
} >> "$GITHUB_ENV"
