#!/usr/bin/env bash
# requires:
# 00_fixture_path_hygiene.sh — a path a fixture writes INTO a file mcpp parses
# must be spelled the way the mcpp BINARY reads it, not the way the shell does.
#
# On Git Bash, `mktemp -d` yields `/tmp/tmp.XXXXXXXX`. MSYS converts POSIX
# paths on the way into argv and the environment; it does not convert file
# CONTENT. A native mcpp.exe therefore reads a literal leading `/` as "root of
# the current drive", and a fixture index at `$TMP/idx` resolves to `C:\tmp\idx`
# — absent. The test then fails as "package not found in any configured index",
# which points nowhere near the fixture.
#
# So this is a lint, not a behaviour test: it runs on every platform (a Linux
# reviewer must be able to catch a Windows-only fixture bug before CI does),
# and it is deliberately mechanical, because this class of mistake is invisible
# in review — `path = "$TMP/idx"` looks correct on the page.
#
# THE RULE
#   A TOML `path = "..."` value that interpolates a shell variable may only
#   interpolate a variable whose name ends in `_HOST` (or `_host`), i.e. one
#   produced by `host_path` from `_host_path.sh`.
#
# Relative values (`path = "../idx"`) are fine and are not matched: they carry
# no drive/root semantics, so both spellings agree.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

# A TOML `path` key: either at the start of a line, or inside an inline table
# (`{ path = "..." }`). Anchoring this way is what keeps shell assignments such
# as `symlink_path="$PKG/..."` and `local path="$1"` out of the match.
readonly TOML_PATH_KEY='(^[[:space:]]*|\{[[:space:]]*)path[[:space:]]*=[[:space:]]*"\$'

violations=0
report() {
    printf '  %s:%s: %s\n' "$1" "$2" "$3"
    violations=$((violations + 1))
}

for f in "$HERE"/[0-9]*.sh; do
    name="$(basename "$f")"
    [[ "$name" == "$(basename "$0")" ]] && continue

    while IFS=: read -r lineno line; do
        [[ -n "${lineno:-}" ]] || continue
        # Extract the interpolated expression: $NAME or ${NAME}.
        var="$(printf '%s' "$line" \
             | sed -nE 's/.*path[[:space:]]*=[[:space:]]*"\$\{?([A-Za-z_][A-Za-z0-9_]*).*/\1/p')"
        if [[ -z "$var" ]]; then
            # `path = "$1"` / `path = "$(...)"` — a positional or a substitution.
            # Neither can be checked by name, so require them to be host-safe at
            # the call site by spelling the value out through a *_HOST variable.
            report "$name" "$lineno" \
                "manifest path interpolates an unnamed expression; assign it to a *_HOST variable produced by host_path"
            continue
        fi
        case "$var" in
            *_HOST | *_host) ;;
            *)
                report "$name" "$lineno" \
                    "manifest path interpolates \$$var; use \$${var}_HOST from host_path (see _host_path.sh)"
                ;;
        esac
    done < <(grep -nE "$TOML_PATH_KEY" "$f" || true)
done

if (( violations > 0 )); then
    echo
    echo "FAIL: $violations fixture path(s) are written in shell spelling, not host spelling."
    echo "      source \"\$(dirname \"\$0\")/_host_path.sh\" and pass the value through host_path."
    exit 1
fi

echo "OK"
