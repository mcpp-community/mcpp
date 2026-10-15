#!/usr/bin/env bash
# O1 (macOS): llvm 23.1.3 installed by xlings $XV into a fresh home.
# O2 (macOS): mcpp $MV with its managed llvm replaced by a symbolic link to
#     the upstream payload: the build, the resource directory, the product.
# Prints observations; never asserts.
set -uo pipefail
. "$(dirname "$0")/lib.sh"
cd "$T"
tree_digest() { (cd "$1" && find . -type f -print0 | sort -z | xargs -0 shasum -a 256 2>/dev/null | shasum -a 256 | cut -c1-16); }

section "host"
obs "sw_vers / xcode" bash -c 'sw_vers; xcode-select -p; xcrun --show-sdk-path'

curl -fsSL -o xl.tgz "https://github.com/openxlings/xlings/releases/download/v$XV/xlings-$XV-macosx-arm64.tar.gz"
tar -xzf xl.tgz
UP="$T/xlings-$XV-macosx-arm64"
x() { clean_env XLINGS_HOME="$UP" HOME="$T/home" "$UP/bin/xlings" "$@"; }

section "O1: xlings $XV, a fresh home, llvm 23.1.3"
obs "xlings --version" x --version
obs "self init" x self init
obs "update" x update
OBS_LINES=15 obs "install xim:llvm@23.1.3" x install -y xim:llvm@23.1.3
L="$UP/data/xpkgs/xim-x-llvm/23.1.3"
obs "clang++ and libc++ (dylibs, rpaths)" bash -c "$(declare -f macho); macho '$L/bin/clang++'; macho \$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' '$L/bin/clang++'); for f in \$(find '$L/lib' -maxdepth 1 -name 'libc++*.dylib' | head -3); do macho \$f; done"
obs "clang cfg files" bash -c "for c in '$L'/bin/*.cfg; do echo \"== \$c\"; cat \"\$c\"; done"
hello_cpp "$T/h.cpp"
obs "clang++ (upstream) builds and runs hello, no flags" bash -c "cd '$T' && '$L/bin/clang++' -std=c++23 h.cpp -o h-clang && ./h-clang"
obs "the product" bash -c "$(declare -f macho); macho '$T/h-clang'"

section "O2: mcpp $MV, managed llvm replaced by a link to the upstream payload"
curl -fsSL -o mc.tgz "https://github.com/mcpp-community/mcpp/releases/download/v$MV/mcpp-$MV-macosx-arm64.tar.gz"
tar -xzf mc.tgz
M="$T/mcpp-$MV-macosx-arm64/bin/mcpp"
export MCPP_HOME="$T/mh"
m() { clean_env MCPP_HOME="$MCPP_HOME" HOME="$T/home" "$M" "$@"; }
OBS_LINES=10 obs "mcpp toolchain install llvm 23.1.3" m toolchain install llvm 23.1.3
obs "mcpp toolchain default llvm@23.1.3" m toolchain default llvm@23.1.3
mkdir -p "$T/w" && cd "$T/w"
obs "mcpp new demo" m new demo
cd "$T/w/demo"
OBS_LINES=10 obs "baseline: build" m build
obs "baseline: run" m run
obs "baseline: product" bash -c "$(declare -f macho); macho \$(find '$T/w/demo/target' -type f -name demo -perm -u+x | head -1)"
RL="$MCPP_HOME/registry/data/xpkgs/xim-x-llvm/23.1.3"
obs "resource dir (mcpp's own payload)" "$RL/bin/clang++" -print-resource-dir
obs "mcpp-modified cfg (comparison)" bash -c "cat '$RL/bin/clang++.cfg' 2>&1 | head -20"
# Round 2: simulate the port's answer "installed" with mcpp's own marker, then
# make the upstream read-only so that any write is visible.
touch "$L/.mcpp_ok"; chmod -R a-w "$L"
LUP_BEFORE="$(tree_digest "$L")"
mv "$RL" "$RL.own" && ln -s "$L" "$RL"
obs "resource dir through the link" "$RL/bin/clang++" -print-resource-dir
rm -rf "$T/w/demo/target"
OBS_LINES=30 obs "llvm linked: build -v" m -v build
obs "llvm linked: is the entry still a link?" ls -ld "$RL"
obs "llvm linked: run" m run
obs "llvm linked: product" bash -c "$(declare -f macho); macho \$(find '$T/w/demo/target' -type f -name demo -perm -u+x | head -1)"
obs "llvm linked: flags in build.ninja (sysroot, -isystem, -L, rpath)" bash -c "grep -rhoE -- '(--sysroot[= ][^ ]+|-isystem [^ ]+|-L[^ ]+|-rpath[, ][^ ]+|-resource-dir[= ][^ ]+)' '$T/w/demo/target' 2>/dev/null | sort | uniq -c | head -20"
obs "was the upstream llvm written?" bash -c "echo before=$LUP_BEFORE after=\$( cd '$L' && find . -type f -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256 | cut -c1-16)"
