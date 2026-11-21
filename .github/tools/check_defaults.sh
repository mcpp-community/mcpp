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
# superseded (mcpp#988). This lists every default beside the newest release of
# its package in an xim-pkgindex checkout and fails when one is behind. The
# newest release of a package is the newest that every platform its defaults
# name publishes (docs/51): a version one of them lacks is not a default.
#
# A release key is a version without a prerelease part; an mcpp revision of an
# upstream release (`1.12.1-mcpp.1`) counts as that release. A toolchain request
# names its payload: `gcc@<v>-musl` is `musl-gcc@<v>`, and gcc for a
# `*-windows-gnu` target is `mingw-gcc`.
#
# Usage: bash .github/tools/check_defaults.sh <xim-pkgindex checkout> [repo_dir]

set -uo pipefail
# A host pattern (`linux-*`, `*`) is a word, never a file name.
set -f

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

# The release keys the descriptor of `pkg` lists, one `<platform> <release>` per
# line: each version key under `linux = {`, `macosx = {` or `windows = {` of the
# descriptor, with its release part.
releases_by_platform() {
    local file="$INDEX/pkgs/${1:0:1}/$1.lua"
    [ -f "$file" ] || return
    awk '
        { sub(/--.*/, "") }
        match($0, /^[ \t]*(linux|macosx|windows)[ \t]*=[ \t]*\{/) {
            platform = $0; sub(/^[ \t]*/, "", platform); sub(/[ \t=].*/, "", platform); at = depth
        }
        platform != "" && match($0, /\["[0-9][0-9A-Za-z.+-]*"\][ \t]*=/) {
            key = substr($0, RSTART + 2); sub(/".*/, "", key); print platform, key
        }
        {
            n = gsub(/\{/, "{"); m = gsub(/\}/, "}"); depth += n - m
            if (platform != "" && depth <= at) platform = ""
        }' "$file" | while read -r platform v; do
        r=$(release_of "$v"); [ -n "$r" ] && echo "$platform $r"
    done
}

# The platforms of the descriptor that the hosts of a default name: `linux-*` and
# `linux-x86_64` are linux, `macos-*` macosx, `windows-*` windows, `*` every one.
platforms_of() {
    local hosts="$1" pkg="$2"
    for h in $hosts; do
        case "$h" in
            linux-*)   echo linux ;;
            macos-*)   echo macosx ;;
            windows-*) echo windows ;;
            '*')       releases_by_platform "$pkg" | awk '{print $1}' ;;
        esac
    done | sort -u
}

# The rows of the built-in table: `<of> <use> <target> <hosts...>`.
rows() {
    awk '
        function flush() { if (use != "") print of, use, (target == "" ? "-" : target), hosts; of = ""; use = ""; target = ""; hosts = "" }
        /^\[\[default\]\]/ { flush() }
        /^of *=/     { gsub(/"/, "", $3); of = $3 }
        /^use *=/    { gsub(/"/, "", $3); use = $3 }
        /^target *=/ { gsub(/"/, "", $3); target = $3 }
        /^hosts *=/  { h = $0; sub(/^[^[]*\[/, "", h); sub(/\].*/, "", h); gsub(/[",]/, " ", h); hosts = h }
        END { flush() }' "$DATA"
}

# The payload a toolchain request names: `gcc@<v>-musl` is `musl-gcc@<v>`, and gcc
# for a `*-windows-gnu` target is `mingw-gcc`.
payload_of() {
    local use="$1" target="$2" pkg="${1%@*}" ver="${1##*@}"
    case "$ver" in *-musl) pkg="musl-$pkg"; ver="${ver%-musl}" ;; esac
    case "$pkg:$target" in gcc:*-windows-gnu) pkg="mingw-gcc" ;; esac
    echo "$pkg $ver"
}

# ONE VERSION OF A PACKAGE FOR EVERY HOST ITS DEFAULTS SERVE: a package's newest
# release is the newest that each platform its defaults name publishes. llvm
# 23.1.6 has no macOS payload (llvm-project's release publishes no macOS binary),
# and the llvm defaults include macOS, so llvm's is 23.1.3 until one exists.
declare -A platforms
while read -r of use target hosts; do
    read -r pkg ver < <(payload_of "$use" "$target")
    platforms[$pkg]+=" $(platforms_of "$hosts" "$pkg" | tr '\n' ' ')"
done < <(rows)

newest_common() {
    local pkg="$1" wanted
    wanted=$(echo "${platforms[$pkg]}" | tr ' ' '\n' | grep -v '^$' | sort -u)
    [ -n "$wanted" ] || return
    local count; count=$(echo "$wanted" | wc -l)
    releases_by_platform "$pkg" | grep -wFf <(echo "$wanted") | sort -u \
        | awk -v need="$count" '{seen[$2]++} END {for (v in seen) if (seen[v] == need) print v}' \
        | sort -V | tail -1
}

fail=0
printf '%-10s %-24s %-16s %s\n' "default" "use" "newest release" "state"
while read -r of use target hosts; do
    read -r pkg ver < <(payload_of "$use" "$target")
    have=$(release_of "$ver")
    newest=$(newest_common "$pkg")
    if [ -z "$newest" ]; then
        state="not in the index"; fail=1
    elif [ "$(printf '%s\n%s\n' "$have" "$newest" | sort -V | tail -1)" != "$have" ]; then
        state="behind"; fail=1
    else
        state="current"
    fi
    printf '%-10s %-24s %-16s %s\n' "$of" "$pkg@$ver" "${newest:--}" "$state"
done < <(rows | sort -u)

if [ "$fail" -ne 0 ]; then
    echo "a default is behind the index or absent from it: move it (docs/51), or publish the payload first"
fi
exit "$fail"
