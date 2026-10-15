#!/usr/bin/env bash
# O1: toolchains installed by xlings $XV into a fresh home -- do they run, and
#     run what they build, with no modification from mcpp?
# O2: mcpp $MV with its managed gcc/llvm replaced by a symbolic link to the
#     upstream home's payload (what a borrowed entry is) -- does the build
#     work, what loader does the product get, is the upstream written?
# Prints observations; never asserts.
set -uo pipefail
. "$(dirname "$0")/lib.sh"
ARCH="$(uname -m)"
cd "$T"

section "host"
obs "uname / glibc" bash -c 'uname -a; ldd --version | head -1'

curl -fsSL -o xl.tgz "https://github.com/openxlings/xlings/releases/download/v$XV/xlings-$XV-linux-$ARCH.tar.gz"
tar -xzf xl.tgz
UP="$T/xlings-$XV-linux-$ARCH"
x() { clean_env XLINGS_HOME="$UP" HOME="$T/home" "$UP/bin/xlings" "$@"; }

section "O1: xlings $XV, a fresh home, gcc 16.1.0"
obs "xlings --version" x --version
obs "self init" x self init
obs "update" x update
OBS_LINES=15 obs "install xim:gcc@16.1.0" x install -y xim:gcc@16.1.0
G="$UP/data/xpkgs/xim-x-gcc/16.1.0"
obs "abi" bash -c "ls -la '$UP/abi' '$UP/abi/'* 2>&1 | head -20; cat '$UP/abi.json' 2>&1 | head -30"
obs "installed xim payloads" bash -c "ls '$UP/data/xpkgs'; for d in '$UP'/data/xpkgs/xim-x-{gcc,binutils,glibc}; do echo \$d: \$(ls \$d 2>&1); done"
CC1="$(find "$G/libexec" -name cc1plus 2>/dev/null | head -1)"
LD="$(ls -d "$UP"/data/xpkgs/xim-x-binutils/*/bin/ld 2>/dev/null | head -1)"
obs "gcc ELF loader/runpath" bash -c "$(declare -f elf); for f in '$G/bin/gcc' '$G/bin/g++' '$CC1' '$LD'; do elf \"\$f\"; done"
obs "gcc specs: dynamic linker and rpath" bash -c "'$G/bin/g++' -dumpspecs | grep -nE 'dynamic-linker|rpath' | head -10; ls '$G'/lib/gcc/*/16.1.0/specs 2>&1; grep -nE 'dynamic-linker|rpath' '$G'/lib/gcc/*/16.1.0/specs 2>/dev/null | head"
hello_cpp "$T/h.cpp"
obs "g++ (upstream) builds and runs hello, no flags" bash -c "cd '$T' && '$G/bin/g++' -std=c++23 h.cpp -o h-gcc && ./h-gcc"
obs "the product's loader/runpath" bash -c "$(declare -f elf); elf '$T/h-gcc'"

section "O1: xlings $XV, llvm 23.1.3"
OBS_LINES=15 obs "install xim:llvm@23.1.3" x install -y xim:llvm@23.1.3
L="$UP/data/xpkgs/xim-x-llvm/23.1.3"
obs "llvm ELF loader/runpath" bash -c "$(declare -f elf); elf '$L/bin/clang++'; elf \$(readlink -f '$L/bin/clang++'); for f in \$(find '$L/lib' -maxdepth 2 -name 'libc++.so.1*' -o -maxdepth 2 -name 'libunwind.so.1*' | head -4); do elf \$f; done"
obs "clang cfg files" bash -c "for c in '$L'/bin/*.cfg; do echo \"== \$c\"; cat \"\$c\"; done"
obs "clang++ (upstream) builds and runs hello, no flags" bash -c "cd '$T' && '$L/bin/clang++' -std=c++23 h.cpp -o h-clang && ./h-clang"
obs "the product's loader/runpath" bash -c "$(declare -f elf); elf '$T/h-clang'"

section "O5 setup: the upstream store becomes root-owned and read-only (a system layer)"
obs "chown root, remove write" bash -c "sudo chown -R root:root '$UP/data/xpkgs' && sudo chmod -R a-w '$UP/data/xpkgs' && stat -c '%U %A %n' '$UP/data/xpkgs/xim-x-gcc/16.1.0' '$UP/data/xpkgs/xim-x-llvm/23.1.3'"

section "O2: mcpp $MV, managed gcc replaced by a link to the upstream payload"
curl -fsSL -o mc.tgz "https://github.com/mcpp-community/mcpp/releases/download/v$MV/mcpp-$MV-linux-$ARCH.tar.gz"
tar -xzf mc.tgz
M="$T/mcpp-$MV-linux-$ARCH/bin/mcpp"
export MCPP_HOME="$T/mh"
m() { clean_env MCPP_HOME="$MCPP_HOME" HOME="$T/home" "$M" "$@"; }
OBS_LINES=10 obs "mcpp toolchain install gcc 16.1.0" m toolchain install gcc 16.1.0
obs "mcpp toolchain default gcc@16.1.0" m toolchain default gcc@16.1.0
mkdir -p "$T/w" && cd "$T/w"
obs "mcpp new demo" m new demo
cd "$T/w/demo"
product() { find "$T/w/demo/target" -type f -name demo -perm -u+x 2>/dev/null | head -1; }
OBS_LINES=10 obs "baseline: build" m build
obs "baseline: run" m run
obs "baseline: product" bash -c "$(declare -f elf); elf \$(find '$T/w/demo/target' -type f -name demo -perm -u+x | head -1)"
R="$MCPP_HOME/registry/data/xpkgs/xim-x-gcc/16.1.0"
obs "mcpp-modified gcc (for comparison)" bash -c "$(declare -f elf); elf '$R/bin/gcc'"
obs "binutils versions: mcpp vs upstream" bash -c "ls '$MCPP_HOME/registry/data/xpkgs/xim-x-binutils' '$UP/data/xpkgs/xim-x-binutils'"
UPD_BEFORE="$(tree_digest "$G")"
mv "$R" "$R.own" && ln -s "$G" "$R"
rm -rf "$T/w/demo/target"
OBS_LINES=30 obs "variant A (gcc linked): build -v" m build -v
obs "variant A: run" m run
obs "variant A: product" bash -c "$(declare -f elf); elf \$(find '$T/w/demo/target' -type f -name demo -perm -u+x | head -1)"
obs "variant A: compile/link commands (loader, rpath, -B, specs)" bash -c "grep -rhoE -- '(--dynamic-linker[= ][^ ]+|-rpath[, ][^ ]+|-B[^ ]+|-specs=[^ ]+|--sysroot[= ][^ ]+)' '$T/w/demo/target' 2>/dev/null | sort | uniq -c | head -20"
for b in "$UP"/data/xpkgs/xim-x-binutils/*; do
    v="$(basename "$b")"; RB="$MCPP_HOME/registry/data/xpkgs/xim-x-binutils/$v"
    if [ -d "$RB" ] && [ ! -L "$RB" ]; then mv "$RB" "$RB.own" && ln -s "$b" "$RB"; say "linked binutils $v"; fi
done
rm -rf "$T/w/demo/target"
OBS_LINES=15 obs "variant B (gcc + binutils linked): build" m build
obs "variant B: run" m run
obs "variant B: product" bash -c "$(declare -f elf); elf \$(find '$T/w/demo/target' -type f -name demo -perm -u+x | head -1)"
obs "was the upstream gcc written?" bash -c "echo before=$UPD_BEFORE after=\$( cd '$G' && find . -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16)"

section "O2: mcpp $MV, managed llvm 23.1.3 replaced by a link"
cd "$T/w/demo"
OBS_LINES=10 obs "mcpp toolchain install llvm 23.1.3" m toolchain install llvm 23.1.3
rm -rf "$T/w/demo/target"
OBS_LINES=10 obs "baseline llvm: build" m build --toolchain llvm@23.1.3
obs "baseline llvm: product" bash -c "$(declare -f elf); elf \$(find '$T/w/demo/target' -type f -name demo -perm -u+x | head -1)"
RL="$MCPP_HOME/registry/data/xpkgs/xim-x-llvm/23.1.3"
obs "mcpp-modified llvm libc++ runpath / cfg (comparison)" bash -c "$(declare -f elf); for f in \$(find '$RL/lib' -maxdepth 2 -name 'libc++.so.1*' | head -2); do elf \$f; done; cat '$RL/bin/clang++.cfg' 2>&1 | head -20"
LUP_BEFORE="$(tree_digest "$L")"
mv "$RL" "$RL.own" && ln -s "$L" "$RL"
rm -rf "$T/w/demo/target"
OBS_LINES=30 obs "llvm linked: build -v" m build -v --toolchain llvm@23.1.3
obs "llvm linked: run" bash -c "\$(find '$T/w/demo/target' -type f -name demo -perm -u+x | head -1)"
obs "llvm linked: product" bash -c "$(declare -f elf); elf \$(find '$T/w/demo/target' -type f -name demo -perm -u+x | head -1)"
obs "was the upstream llvm written?" bash -c "echo before=$LUP_BEFORE after=\$( cd '$L' && find . -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16)"
cp -r "$T/w/demo/target" "$T/logs/target-llvm" 2>/dev/null || true
