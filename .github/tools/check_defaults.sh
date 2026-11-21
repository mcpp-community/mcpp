#!/usr/bin/env bash
# check_defaults.sh — each default of the built-in table against the newest
# release the package index publishes (design 2026-11-21, offline and toolchain
# defaults §3.7; the release procedure, .agents/skills/mcpp-release).
#
# WHY THIS EXISTS
#
# A default is the newest release of its upstream when mcpp is released. The
# defaults are data (`[[default]]` of modules/toolchain-model/data/builtin.toml),
# and nothing compared them with what the index publishes: binutils 2.42 and
# patchelf 0.18.0 stayed defaults through releases that their upstreams had
# superseded (mcpp#988). This lists every default beside the newest release key
# of its package in an xim-pkgindex checkout and fails when one is behind.
#
# A release key is a version without a prerelease part; an mcpp revision of an
# upstream release (`1.12.1-mcpp.1`) counts as that release. A toolchain request
# names its payload: `gcc@<v>-musl` is `musl-gcc@<v>`, and gcc for a
# `*-windows-gnu` target is `mingw-gcc`.
#
# Usage: bash .github/tools/check_defaults.sh <xim-pkgindex checkout> [repo_dir]

set -uo pipefail

INDEX="${1:?usage: check_defaults.sh <xim-pkgindex checkout> [repo_dir]}"
ROOT="${2:-$(cd "$(dirname "$0")/../.." && pwd)}"
DATA="$ROOT/modules/toolchain-model/data/builtin.toml"
[ -f "$DATA" ] || { echo "error: $DATA not found"; exit 2; }
[ -d "$INDEX/pkgs" ] || { echo "error: $INDEX is not an xim-pkgindex checkout (no pkgs/)"; exit 2; }

# The release part of a version: `1.12.1-mcpp.1` -> 1.12.1; a prerelease
# (`17.0.0-20261108`) has none.
release_of() {
    case "$1" in
        *-mcpp.*) echo "${1%%-mcpp.*}" ;;
        *-*)      echo "" ;;
        *)        echo "$1" ;;
    esac
}

# The newest release key the descriptor of `pkg` lists.
newest_release() {
    local file="$INDEX/pkgs/${1:0:1}/$1.lua"
    [ -f "$file" ] || { echo ""; return; }
    grep -oE '\["[0-9][0-9A-Za-z.+-]*"\] *=' "$file" | sed -E 's/\["([^"]*)"\].*/\1/' \
        | while read -r v; do r=$(release_of "$v"); [ -n "$r" ] && echo "$r"; done \
        | sort -uV | tail -1
}

fail=0
printf '%-10s %-24s %-16s %s\n' "default" "use" "newest release" "state"
while read -r of use target; do
    pkg="${use%@*}"; ver="${use##*@}"
    case "$ver" in *-musl) pkg="musl-$pkg"; ver="${ver%-musl}" ;; esac
    case "$pkg:$target" in gcc:*-windows-gnu) pkg="mingw-gcc" ;; esac
    have=$(release_of "$ver")
    newest=$(newest_release "$pkg")
    if [ -z "$newest" ]; then
        state="not in the index"; fail=1
    elif [ "$(printf '%s\n%s\n' "$have" "$newest" | sort -V | tail -1)" != "$have" ]; then
        state="behind"; fail=1
    else
        state="current"
    fi
    printf '%-10s %-24s %-16s %s\n' "$of" "$pkg@$ver" "${newest:--}" "$state"
done < <(awk '
    function flush() { if (use != "") print of, use, target; of = ""; use = ""; target = "" }
    /^\[\[default\]\]/ { flush() }
    /^of *=/     { gsub(/"/, "", $3); of = $3 }
    /^use *=/    { gsub(/"/, "", $3); use = $3 }
    /^target *=/ { gsub(/"/, "", $3); target = $3 }
    END { flush() }' "$DATA" | sort -u)

if [ "$fail" -ne 0 ]; then
    echo "a default is behind the index or absent from it: move it (docs/51), or publish the payload first"
fi
exit "$fail"
