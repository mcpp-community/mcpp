#!/usr/bin/env bash
# requires:
# 47_cdb_prebuilt_module_path_abs.sh — `-fprebuilt-module-path` in
# compile_commands.json must be an ABSOLUTE path, not a bare `pcm.cache` /
# `gcm.cache`. Reason: CDB `directory` is the project root, but ninja runs
# from the outputDir; a bare relative path works at build time only and
# silently breaks clangd's module resolution ("module 'X' not found").
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new app > /dev/null
cd app
"$MCPP" build > /dev/null

cdb=compile_commands.json
[[ -f "$cdb" ]] || { echo "FAIL: no $cdb generated"; exit 1; }

# Extract every -fprebuilt-module-path=<value> token.
# (No jq dependency — grep is enough and portable on macOS / git-bash.)
vals=$(grep -oE '\-fprebuilt-module-path=[^"]+' "$cdb" | sed 's/^-fprebuilt-module-path=//')
if [[ -z "$vals" ]]; then
    # No -fprebuilt-module-path emitted = GCC toolchain that uses -fmodules
    # only (gcm.cache). bmi_traits sets needsPrebuiltModulePath=false for
    # the GCC path. Nothing to assert — pass.
    echo "OK (no prebuilt-module-path flag — GCC toolchain)"
    exit 0
fi

# Every value must be absolute AND point at the actual build cache dir.
fail=0
while read -r v; do
    [[ -z "$v" ]] && continue
    echo "  checking: $v"

    # Absolute path test: leading '/' on POSIX or '<letter>:' on Windows.
    if [[ "$v" =~ ^/ || "$v" =~ ^[A-Za-z]: ]]; then
        :
    else
        echo "FAIL: -fprebuilt-module-path value is relative: '$v'"
        echo "  this breaks clangd because CDB 'directory' = project root,"
        echo "  but the BMI cache lives under target/<triple>/<fp>/."
        fail=1
    fi

    # Must end with pcm.cache or gcm.cache (sanity).
    case "$v" in
        */pcm.cache|*/gcm.cache) ;;
        *)  echo "FAIL: -fprebuilt-module-path value does not end in pcm.cache/gcm.cache: '$v'"
            fail=1 ;;
    esac
done <<< "$vals"

[[ $fail -eq 0 ]] || exit 1

# Stronger: clangd would `cd` into CDB's directory then resolve. Verify the
# value, taken at face value (already absolute), points at a real dir.
first=$(echo "$vals" | head -1)
if [[ ! -d "$first" ]]; then
    echo "FAIL: -fprebuilt-module-path resolves to a non-existent dir: $first"
    echo "  (build succeeded, so the BMI dir must exist somewhere — this"
    echo "   means the flag points to the wrong place even with abs path.)"
    exit 1
fi

echo "OK"
