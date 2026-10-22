#!/usr/bin/env bash
# Repro for 2026-10-23-target-base-and-execution-host-design.md §1.
#
# Builds, from the payloads an mcpp home already holds:
#   toolchain/          gcc 16.1.0 + binutils 2.42.1 in one prefix, pristine specs
#                       (a "vendor" gcc: not a path inside mcpp's registry)
#   xgcc/               aarch64-linux-musl-gcc 16.1.0 (a cross toolchain carrying a sysroot)
#   sysroot-glibc244/   glibc 2.44.3 + linux-headers, crosstool-ng layout
#   sysroot-glibc231/   Debian 11 glibc 2.31 (only with FETCH_DEBIAN=1; needs network)
# and runs the scenario matrix A-K against $MCPP (J and K drive the compilers directly:
# mcpp has no spelling for a host sysroot yet).
#
# Usage: MCPP=/path/to/mcpp [FETCH_DEBIAN=1] [WORK=/tmp/sysroot-demo] bash this.sh
set -uo pipefail
MCPP=${MCPP:?set MCPP to the mcpp binary under test}
WORK=${WORK:-/tmp/sysroot-demo}
R=${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs
GCC=$R/xim-x-gcc/16.1.0
BINUTILS=$(ls -d $R/xim-x-binutils/* | tail -1)
GLIBC=$R/xim-x-glibc/2.44.3
HEADERS=$(ls -d $R/xim-x-linux-headers/* | tail -1)
XGCC=$(ls -d $R/xim-x-aarch64-linux-musl-gcc/* | tail -1)
export MCPP_NO_AUTO_INSTALL=1 MCPP_JOBS=${MCPP_JOBS:-8}

mkdir -p "$WORK"
T=$WORK/toolchain S=$WORK/sysroot-glibc244 X=$WORK/xgcc

# ── toolchains ─────────────────────────────────────────────────────────────
rm -rf "$T" "$X"
cp -a "$GCC" "$T"; chmod -R u+w "$T"
rm -f "$T"/lib/gcc/x86_64-linux-gnu/*/specs           # the registry copy carries baked specs
mkdir -p "$T/x86_64-linux-gnu/bin"
for t in as ld ld.bfd ar nm ranlib objcopy objdump strip readelf; do
  cp -a "$BINUTILS/bin/$t" "$T/bin/$t"
  ln -sf "../../bin/$t" "$T/x86_64-linux-gnu/bin/$t"
done
cp -a "$XGCC" "$X"; chmod -R u+w "$X"

# ── sysroots (crosstool-ng layout: usr/include, usr/lib64, lib64 -> usr/lib64) ─
layout() { mkdir -p "$1/usr/include" "$1/usr/lib64" "$1/lib" "$1/usr/lib"; ln -sfn usr/lib64 "$1/lib64"; }
relink_scripts() {  # $1 sysroot, $2 sed expression; text-only files (-I)
  for f in $(grep -lI "GROUP\|INPUT" "$1"/usr/lib64/*.so 2>/dev/null); do sed -i "$2" "$f"; done
}
rm -rf "$S"; layout "$S"
cp -a "$GLIBC/include/." "$HEADERS/include/." "$S/usr/include/"
cp -a "$GLIBC/lib/." "$S/usr/lib64/"; chmod -R u+w "$S"
relink_scripts "$S" "s#$GLIBC/lib/#/lib64/#g"

if [ "${FETCH_DEBIAN:-0}" = 1 ]; then
  O=$WORK/sysroot-glibc231; rm -rf "$O"; mkdir -p "$O/x" "$WORK/dl"
  for f in g/glibc/libc6_2.31-13+deb11u11_amd64.deb g/glibc/libc6-dev_2.31-13+deb11u11_amd64.deb \
           l/linux/linux-libc-dev_5.10.223-1_amd64.deb; do
    [ -f "$WORK/dl/${f##*/}" ] || curl -sSfL -o "$WORK/dl/${f##*/}" "https://deb.debian.org/debian/pool/main/$f"
    dpkg-deb -x "$WORK/dl/${f##*/}" "$O/x"
  done
  layout "$O"
  cp -a "$O/x/usr/include/." "$O/x/usr/include/x86_64-linux-gnu/." "$O/usr/include/"
  cp -a "$O/x/lib/x86_64-linux-gnu/." "$O/x/usr/lib/x86_64-linux-gnu/." "$O/usr/lib64/"
  rm -rf "$O/x"
  relink_scripts "$O" "s#/usr/lib/x86_64-linux-gnu/#/lib64/#g; s#/lib/x86_64-linux-gnu/#/lib64/#g"
  for l in $(find "$O/usr/lib64" -maxdepth 1 -type l); do
    t=$(readlink "$l"); case $t in /lib/x86_64-linux-gnu/*) ln -sfn "$(basename "$t")" "$l";; esac
  done
fi

# ── sources ────────────────────────────────────────────────────────────────
GLIBC_MAIN='#include <gnu/libc-version.h>
#include <pthread.h>
import std;
int main() {
    std::println("compiled against glibc {}.{}", __GLIBC__, __GLIBC_MINOR__);
    std::println("running on      glibc {}", gnu_get_libc_version());
#if __GLIBC_PREREQ(2, 42)
    std::println("pthread_gettid_np = {}", pthread_gettid_np(pthread_self()));   // GLIBC_2.42
#endif
}'
PLAIN_MAIN='import std;
int main() { std::println("hello from a {}-bit target", sizeof(void*) * 8); }'

scenario() {  # name, main, manifest-tail, [build args]
  local d=$WORK/m/$1; rm -rf "$d"; mkdir -p "$d/src"
  printf '%s\n' "$2" > "$d/src/main.cpp"
  printf '[package]\nname = "m%s"\nversion = "0.1.0"\n\n%s\n' "$1" "$3" > "$d/mcpp.toml"
  echo "================ $1 ${4:-}"; sed -n '5,$p' "$d/mcpp.toml"
  ( cd "$d" && nice -n 10 "$MCPP" build ${4:-} 2>&1 | grep -v '^\s*\(Index\|Inferred\)' | tail -6 )
  local b; b=$(ls "$d"/target/*/*/bin/m$1 2>/dev/null | head -1)
  [ -n "$b" ] || return 0
  file "$b" | cut -d, -f1-2
  readelf -lW "$b" 2>/dev/null | grep -o 'interpreter: [^]]*'
  readelf -dW "$b" 2>/dev/null | grep -i 'rpath\|runpath'
  file "$b" | grep -q x86-64 && { echo "-- run:"; "$b" 2>&1 | head -3; }
}

scenario A "$GLIBC_MAIN" "[toolchain.linux]
path = \"$T\""
scenario B "$GLIBC_MAIN" "[toolchain.linux]
path = \"$T\"
prefix = \"x86_64-linux-gnu-\""
scenario C "$GLIBC_MAIN" "[toolchain.linux]
path    = \"$T\"
sysroot = \"$S\""
scenario D "$GLIBC_MAIN" "[toolchain.linux]
path    = \"$T\"
sysroot = \"$S\"

[build]
ldflags = [\"-Wl,--sysroot=$S\"]"
scenario E "$GLIBC_MAIN" "[toolchain.linux]
path    = \"$T\"
sysroot = \"$S\"

[build]
ldflags = [\"-Wl,--sysroot=$S\", \"-Wl,--dynamic-linker=$S/lib64/ld-linux-x86-64.so.2\", \"-Wl,-rpath,$S/lib64\"]"
scenario F "$GLIBC_MAIN" "[toolchain]
linux = \"gcc@16.1.0\"

[target.x86_64-linux-gnu]
sysroot = \"$S\""
scenario G "$PLAIN_MAIN" "[toolchain.linux]
path   = \"$X\"
prefix = \"aarch64-linux-musl-\""
scenario H "$PLAIN_MAIN" "[toolchain.linux]
path    = \"$X\"
prefix  = \"aarch64-linux-musl-\"
sysroot = \"$X/aarch64-linux-musl\""
if [ -d "$WORK/sysroot-glibc231" ]; then
  scenario I "$PLAIN_MAIN" "[toolchain.linux]
path    = \"$T\"
sysroot = \"$WORK/sysroot-glibc231\""
fi

# ── J / K: the host's own root as the sysroot (Debian/Ubuntu multiarch) ─────
# Not through mcpp: `glibc@system` is what the design adds. The ecosystem gcc is
# built without multiarch (-print-multiarch is empty), so J fails until the
# multiarch directories are wired; clang (K) resolves them itself.
LLVM=$(ls -d $R/xim-x-llvm/* 2>/dev/null | tail -1)
TUPLE=$(/usr/bin/gcc -print-multiarch 2>/dev/null); [ -n "$TUPLE" ] || TUPLE=$(uname -m)-linux-gnu
mkdir -p "$WORK/m/J"; printf '%s\n' '#include <gnu/libc-version.h>' '#include <stdio.h>' \
  'int main(){printf("compiled against %d.%d, running on %s\n",__GLIBC__,__GLIBC_MINOR__,gnu_get_libc_version());}' > "$WORK/m/J/c.c"
echo "================ J (gcc --sysroot=/)"
"$T/bin/gcc" --sysroot=/ "$WORK/m/J/c.c" -o "$WORK/m/J/plain" 2>&1 | tail -2
echo "-- with the multiarch directories wired:"
"$T/bin/gcc" --sysroot=/ -B/usr/lib/$TUPLE -L/usr/lib/$TUPLE -L/lib/$TUPLE -isystem /usr/include/$TUPLE \
  -Wl,-rpath-link,/lib/$TUPLE "$WORK/m/J/c.c" -o "$WORK/m/J/wired" 2>&1 | tail -2 && "$WORK/m/J/wired"
if [ -n "$LLVM" ]; then
  echo "================ K (clang --sysroot=/)"
  "$LLVM/bin/clang" --no-default-config --sysroot=/ "$WORK/m/J/c.c" -o "$WORK/m/J/clang" 2>&1 | tail -2 && "$WORK/m/J/clang"
fi
