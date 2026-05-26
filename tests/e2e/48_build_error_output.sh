#!/usr/bin/env bash
# requires:
# Default build failures should show the compiler diagnostic once, without
# ninja progress or private toolchain environment prefixes.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<'EOF'
[package]
name    = "bad_error_output"
version = "0.1.0"

[language]
import_std = false
EOF

cat > src/main.cpp <<'EOF'
#error MCPPE2E_BUILD_ERROR_MARKER
int main() {
    return 0;
}
EOF

rc=0
out=$("$MCPP" build 2>&1) || rc=$?
[[ $rc -ne 0 ]] || {
    echo "expected build to fail"
    echo "$out"
    exit 1
}

echo "$out" | grep -q 'MCPPE2E_BUILD_ERROR_MARKER' || {
    echo "missing compiler diagnostic marker"
    echo "$out"
    exit 1
}

compiling_line=$(printf '%s\n' "$out" | awk '/Compiling bad_error_output/ { print NR; exit }')
error_line=$(printf '%s\n' "$out" | awk '/^error: build failed/ { print NR; exit }')
[[ -n "$compiling_line" && -n "$error_line" && "$compiling_line" -lt "$error_line" ]] || {
    echo "expected build status before diagnostics"
    echo "$out"
    exit 1
}

diag_count=$(echo "$out" | grep -c 'error:.*MCPPE2E_BUILD_ERROR_MARKER' || true)
[[ "$diag_count" -eq 1 ]] || {
    echo "expected compiler diagnostic marker once, got $diag_count"
    echo "$out"
    exit 1
}

duplicate_lines=$(printf '%s\n' "$out" | awk 'NF && seen[$0]++ == 1 { print }')
[[ -z "$duplicate_lines" ]] || {
    echo "unexpected duplicate diagnostic lines"
    echo "$duplicate_lines"
    echo "$out"
    exit 1
}

for noise in \
    'LD_LIBRARY_PATH=' \
    'FAILED:' \
    'ninja: Entering directory' \
    'ninja: build stopped' \
    'argument unused during compilation' \
    '^\[[0-9][0-9]*/[0-9][0-9]*\] '
do
    if echo "$out" | grep -Eq "$noise"; then
        echo "unexpected build-output noise matched: $noise"
        echo "$out"
        exit 1
    fi
done

build_ninja=$(find target -name build.ninja | head -1)
[[ -n "$build_ninja" ]] || {
    echo "missing generated build.ninja"
    echo "$out"
    exit 1
}
if grep -q 'LD_LIBRARY_PATH\|toolenv' "$build_ninja"; then
    echo "build.ninja should not contain private runtime env prefixes"
    sed -n '1,80p' "$build_ninja"
    exit 1
fi
if grep '^cxxflags\|^cflags' "$build_ninja" | grep -Eq -- '-stdlib=libc\+\+|-fuse-ld=lld|--rtlib=compiler-rt|--unwindlib=libunwind'; then
    echo "compile flags should not contain clang link/runtime-only flags"
    grep '^cxxflags\|^cflags' "$build_ninja"
    exit 1
fi

rc=0
verbose_out=$("$MCPP" build --verbose 2>&1) || rc=$?
[[ $rc -ne 0 ]] || {
    echo "expected verbose build to fail"
    echo "$verbose_out"
    exit 1
}
echo "$verbose_out" | grep -q 'FAILED:' || {
    echo "verbose output should retain ninja failure details"
    echo "$verbose_out"
    exit 1
}
echo "$verbose_out" | grep -q 'MCPPE2E_BUILD_ERROR_MARKER' || {
    echo "verbose output missing compiler diagnostic marker"
    echo "$verbose_out"
    exit 1
}

echo "OK"
