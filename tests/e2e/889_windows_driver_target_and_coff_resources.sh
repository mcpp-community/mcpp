#!/usr/bin/env bash
# requires: msvc
# mcpp#775: the driver link and COFF resource must target the selected machine.
# 2026.10.5.3: `x86-windows-msvc` is the i686 row.
set -e
source "$(dirname "$0")/_host_path.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"
REGISTRY_HOST=$(host_path "${MCPP_HOME:-$HOME/.mcpp}/registry")
export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"
printf '[xlings]\nhome = "%s"\n' "$REGISTRY_HOST" > "$MCPP_HOME/config.toml"
cd "$TMP"
hexof() { od -An -v -tx1 "$1" | tr -d ' \n'; }
pe_machine() {
    local offset
    offset=$(od -An -tu4 -j60 -N4 "$1" | tr -d ' \n')
    od -An -tu2 -j"$((offset + 4))" -N2 "$1" | tr -d ' \n'
}
# `x86` is MSVC's spelling of i686 (2026.10.5.3): the same row, reached by the
# name an MSVC user writes. Before, it reached clang as `x86-pc-windows-msvc`.
for arch in i686 x86_64 x86; do
    machine=332
    [ "$arch" != x86_64 ] || machine=34404
    cases="c cxx dll"
    [ "$arch" != x86 ] || cases=cxx
    for case_name in $cases; do
        mkdir -p "$arch/$case_name/src"
        cd "$arch/$case_name"
        kind=bin
        suffix=exe
        main_line='main = "src/main.cpp"'
        if [ "$case_name" = dll ]; then
            kind=shared
            suffix=dll
            main_line=''
            printf 'extern "C" __declspec(dllexport) int answer() { return 42; }\n' > src/main.cpp
        elif [ "$case_name" = c ]; then
            main_line='main = "src/main.c"'
            printf 'int main(void) { return 0; }\n' > src/main.c
        else
            printf 'int main() { return 0; }\n' > src/main.cpp
        fi
        cat > mcpp.toml <<EOF
[package]
name = "probe"
version = "0.1.0"
[toolchain]
windows = "llvm@20.1.7"
[build]
cxx_runtime = "host-coupled"
[target.$arch-windows-msvc]
sysroot = "msvc@system"
[targets.probe]
kind = "$kind"
$main_line
windows_auto_export = false
EOF
        # The first build isolates the driver regression from the resource one.
        "$MCPP" build --target "$arch-windows-msvc" > first.log 2>&1 \
            || fail "$arch/$case_name: driver link failed" first.log
        image=$(find target -name "probe.$suffix" | head -1)
        [ -n "$image" ] || fail "$arch/$case_name: image missing" first.log
        [ "$(pe_machine "$image")" = "$machine" ] || fail "wrong PE machine: $image"
        [ "$kind" != bin ] || "$image" || fail "executable failed: $image"
        printf '\n[resources]\nfiles = ["app.rc"]\n' >> mcpp.toml
        printf '101 RCDATA { 0x1357, 0x2468, 0xabcdef01L }\n' > app.rc
        "$MCPP" build --target "$arch-windows-msvc" > resource.log 2>&1 \
            || fail "$arch/$case_name: resource link failed" resource.log
        object=$(find target -path '*/res/*.o' | head -1)
        [ -n "$object" ] || fail "$arch/$case_name: COFF resource missing" resource.log
        [ "$(od -An -tu2 -N2 "$object" | tr -d ' \n')" = "$machine" ] \
            || fail "wrong COFF machine: $object"
        [[ "$(hexof "$image")" == *5713682401efcdab* ]] || fail "resource data missing"
        printf '101 RCDATA { 0x7654, 0x3210, 0xfedcba98L }\n' > app.rc
        "$MCPP" build --target "$arch-windows-msvc" > rebuild.log 2>&1 \
            || fail "$arch/$case_name: resource rebuild failed" rebuild.log
        [ "$(pe_machine "$image")" = "$machine" ] || fail "rebuild changed PE machine"
        [[ "$(hexof "$image")" == *5476103298badcfe* ]] || fail "resource data stayed stale"
        cd "$TMP"
    done
done
echo "PASS: 889 Windows driver target and COFF resources"
