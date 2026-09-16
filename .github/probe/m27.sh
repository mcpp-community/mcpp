#!/usr/bin/env bash
# macOS 27 round 3: which single flag makes libc++ 22's std module build in strict mode.
set +e
r() { echo "READING $*"; }
L="$LLVM_ROOT"; SDK=$(xcrun --show-sdk-path); S=$L/share/libc++/v1/std.cppm; SC=$L/share/libc++/v1/std.compat.cppm
W=$(mktemp -d); cd $W
BASE=(--no-default-config -nostdinc++ -isystem $L/include/c++/v1 -isysroot $SDK -Wno-reserved-module-identifier -std=c++23)
try() { name=$1; shift; out=$("$@" 2>&1); rc=$?; r "$name rc=$rc $(printf '%s' "$out" | grep -m1 -E 'error|warning' | cut -c1-220)"; }
r "modulemap infinity_nan: $(grep -n -A3 'infinity_nan' $($L/bin/clang -print-resource-dir)/include/module.modulemap | tr '\n' ' ')"
r "driver cc1 builtin-headers: $($L/bin/clang++ "${BASE[@]}" -### -fsyntax-only -x c++ /dev/null 2>&1 | tr ' ' '\n' | grep -i 'builtin-headers\|fmodules' | tr '\n' ' ')"
try baseline               $L/bin/clang++ "${BASE[@]}" --precompile $S -o a.pcm
try builtin-in-sysmodules  $L/bin/clang++ "${BASE[@]}" -Xclang -fbuiltin-headers-in-system-modules --precompile $S -o b.pcm
try no-modulemaps          $L/bin/clang++ "${BASE[@]}" -fno-implicit-module-maps --precompile $S -o c.pcm
try need-inf-nan           $L/bin/clang++ "${BASE[@]}" -D__need_infinity_nan --precompile $S -o d.pcm
try defs-sdk-spelling      $L/bin/clang++ "${BASE[@]}" "-DINFINITY=HUGE_VALF" "-DNAN=__builtin_nanf(\"0x7fc00000\")" --precompile $S -o e.pcm
# the winner must also give std.compat and a working program, and must not warn in plain TUs
printf '#include <cmath>\n#include <cfloat>\n#include <math.h>\n#include <float.h>\nint main(){ return INFINITY > 0 && NAN != NAN ? 0 : 1; }\n' > p.cpp
printf '#include <math.h>\n#include <float.h>\nint main(void){ return INFINITY > 0 && NAN != NAN ? 0 : 1; }\n' > p.c
for v in "builtin-in-sysmodules:-Xclang -fbuiltin-headers-in-system-modules" "defs-sdk-spelling:-DINFINITY=HUGE_VALF -DNAN=__builtin_nanf(\"0x7fc00000\")"; do
  name=${v%%:*}; flags=${v#*:}
  eval "F=($flags)"
  try "$name plain-c++ -Werror" $L/bin/clang++ "${BASE[@]}" "${F[@]}" -Werror -Wall p.cpp -o pcpp
  try "$name plain-c -Werror"   $L/bin/clang --no-default-config -isysroot $SDK "${F[@]}" -std=c17 -Werror -Wall p.c -o pc
  try "$name std.compat"        $L/bin/clang++ "${BASE[@]}" "${F[@]}" -fmodule-file=std=a.pcm --precompile $SC -o sc.pcm
  rm -rf prog; mkdir prog; cd prog
  $L/bin/clang++ "${BASE[@]}" "${F[@]}" --precompile $S -o std.pcm >/dev/null 2>&1
  $L/bin/clang++ "${BASE[@]}" "${F[@]}" -c std.pcm -o std.o >/dev/null 2>&1
  printf 'export module mm;\nimport std;\n#include <cmath>\nexport double inf() { return INFINITY; }\n' > mm.cppm
  $L/bin/clang++ "${BASE[@]}" "${F[@]}" -fmodule-file=std=std.pcm --precompile mm.cppm -o mm.pcm 2> mm.err
  $L/bin/clang++ "${BASE[@]}" "${F[@]}" -c mm.pcm -o mm.o >/dev/null 2>&1
  printf 'import std;\nimport mm;\nint main(){ std::println("inf={} nan={}", inf(), std::isnan(std::nan(""))); }\n' > m.cpp
  $L/bin/clang++ "${BASE[@]}" "${F[@]}" -fmodule-file=std=std.pcm -fmodule-file=mm=mm.pcm -c m.cpp -o m.o 2> m.err
  $L/bin/clang++ --no-default-config -isysroot $SDK -fuse-ld=lld -nostdlib++ m.o mm.o std.o $L/lib/libc++.a $L/lib/libc++abi.a -o m 2> l.err
  r "$name program: $(./m 2>&1) | $(head -1 mm.err) $(head -1 m.err) $(head -1 l.err)"
  cd ..
done
# same flags on the macOS 26.5 SDK: must stay green
SDK26=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
B26=(--no-default-config -nostdinc++ -isystem $L/include/c++/v1 -isysroot $SDK26 -Wno-reserved-module-identifier -std=c++23)
try sdk26-builtin-in-sysmodules $L/bin/clang++ "${B26[@]}" -Xclang -fbuiltin-headers-in-system-modules --precompile $S -o f.pcm
try sdk26-plain-c++-Werror      $L/bin/clang++ "${B26[@]}" -Xclang -fbuiltin-headers-in-system-modules -Werror -Wall p.cpp -o p26
exit 0
