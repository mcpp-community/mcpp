#!/usr/bin/env bash
# requires:
# 47_cdb_prebuilt_module_path_abs.sh — `-fprebuilt-module-path` in
# compile_commands.json must be an ABSOLUTE path, NOT a bare `pcm.cache`,
# AND must not carry ninja-escape artefacts like `C$:` on Windows.
# Reason: CDB `directory` is the project root and clangd does `cd
# directory` before running the args, so a bare relative path points at
# `<projectRoot>/pcm.cache` (missing) and a `C$:` prefix is treated as a
# literal string, not a Windows drive letter. Both modes silently break
# clangd's module resolution while `mcpp build` itself keeps working
# (ninja runs from outputDir AND unescapes its own escape sequences).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new app > /dev/null
cd app
"$MCPP" build > /dev/null

cdb=compile_commands.json
[[ -f "$cdb" ]] || { echo "FAIL: no $cdb generated"; exit 1; }

command -v jq >/dev/null 2>&1 || {
    echo "SKIP: jq not on PATH (preinstalled on GitHub-hosted runners)"
    exit 0
}

# jq returns each value JSON-unescaped (\\ → \, etc.). Stage to a temp
# file then read with a redirected while loop — bash 3.2 on macOS lacks
# `mapfile`/`readarray`, and a `| while` pipeline puts the loop in a
# subshell so `fail=1` would not propagate. Input redirection runs the
# loop in the current shell, preserving the flag.
jq -r '
    .[] | .arguments[]?
        | select(type == "string" and startswith("-fprebuilt-module-path="))
        | sub("^-fprebuilt-module-path="; "")
' "$cdb" > "$TMP/vals.txt"

if [[ ! -s "$TMP/vals.txt" ]]; then
    # GCC's libstdc++ flow uses -fmodules / gcm.cache without the explicit
    # -fprebuilt-module-path flag (see bmi_traits.needsPrebuiltModulePath).
    # Nothing to assert in that mode.
    echo "OK (no prebuilt-module-path flag — GCC toolchain)"
    exit 0
fi

fail=0
while IFS= read -r v; do
    # `jq` on git-bash/Windows emits CRLF; strip the trailing CR so basename
    # / regex comparisons don't trip over an invisible `\r`.
    v="${v%$'\r'}"
    [[ -z "$v" ]] && continue
    echo "  checking: $v"

    # Must NOT carry ninja-escape artefacts. The key signal is `$:` (drive
    # letter) or `$ ` / `$$` (path with space / dollar). If any of these
    # survives into CDB the JSON-args runtime treats them as literal text
    # → clangd fails to find the BMI.
    if [[ "$v" == *'$:'* || "$v" == *'$ '* || "$v" == *'$$'* ]]; then
        echo "FAIL: value retains ninja escape sequence ('\$:' / '\$ ' / '\$\$') — must be plain path in CDB"
        fail=1
    fi

    # Absolute: POSIX (starts with '/') or Windows drive (e.g. 'C:').
    if [[ "$v" =~ ^/ || "$v" =~ ^[A-Za-z]: ]]; then
        :
    else
        echo "FAIL: value is relative: '$v'"
        echo "      CDB 'directory' is the project root, but the BMI cache"
        echo "      lives under target/<triple>/<fp>/ — clangd resolves to"
        echo "      the wrong location and module imports fail."
        fail=1
    fi

    # Basename must be pcm.cache or gcm.cache (cross-platform: normalise
    # backslashes first so Windows paths like 'C:\foo\pcm.cache' work).
    normalised="${v//\\//}"
    case "${normalised##*/}" in
        pcm.cache|gcm.cache) ;;
        *)  echo "FAIL: basename is not pcm.cache/gcm.cache: '${normalised##*/}'"
            fail=1 ;;
    esac
done < "$TMP/vals.txt"

[[ $fail -eq 0 ]] || exit 1
echo "OK"
