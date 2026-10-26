#!/usr/bin/env bash
# Builds app-<arch>.oke: ELF ET_DYN, no operating system, imports kal_* only.
# usage: build.sh <llvm bin dir> <out dir>
set -euo pipefail
LLVM="$1"; OUT="$2"; HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
for arch in x86_64 aarch64; do
  extra=""
  [ "$arch" = aarch64 ] && extra="-ffixed-x18 -mno-outline-atomics"
  "$LLVM/clang" --target=$arch-unknown-none-elf -ffreestanding -fPIC -fvisibility=hidden \
      -femulated-tls -fno-stack-protector -fno-builtin -O2 $extra \
      -c "$HERE/app.c" -o "$OUT/app-$arch.o"
  "$LLVM/ld.lld" -shared -z max-page-size=65536 -z noexecstack -z now --hash-style=both \
      --no-undefined-version -o "$OUT/app-$arch.oke" "$OUT/app-$arch.o"
  echo "== $arch"
  "$LLVM/llvm-readelf" -h -l -d --dyn-syms "$OUT/app-$arch.oke" | grep -E "Type:|Machine:|LOAD|NEEDED|FLAGS|UND|ok_main|RELA|JMPREL" | head -30
  sha256sum "$OUT/app-$arch.oke"
done
