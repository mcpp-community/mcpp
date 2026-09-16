#!/usr/bin/env bash
# macOS 27: libc++ 22 std.cppm loses INFINITY/NAN. Round 2: mechanism and candidate fixes.
set +e
r() { echo "READING $*"; }
L="$LLVM_ROOT"; SDK=$(xcrun --show-sdk-path); S=$L/share/libc++/v1/std.cppm
r "llvm=$L cfg=$(tr '\n' ' ' < $L/bin/clang++.cfg)"
r "math.h 55-95:"; sed -n 55,95p $SDK/usr/include/math.h | sed 's/^/READING   math.h| /'
FH=$($L/bin/clang -print-resource-dir)/include/float.h
r "clang float.h INFINITY region:"; grep -n -B4 -A3 'define INFINITY' $FH | sed 's/^/READING   float.h| /'
AF=$(/usr/bin/clang -print-resource-dir)/include/float.h
r "apple float.h INFINITY region:"; grep -n -B4 -A3 'define INFINITY' $AF | sed 's/^/READING   apple-float.h| /'
W=$(mktemp -d); cd $W
BASE=(--no-default-config -nostdinc++ -isystem $L/include/c++/v1 -isysroot $SDK -Wno-reserved-module-identifier)
try() { name=$1; shift; out=$("$@" 2>&1); rc=$?; r "$name rc=$rc $(printf '%s' "$out" | grep -m1 'error' | cut -c1-200)"; }
printf 'int main(){ return __has_feature(modules); }\n' > hf.cpp
try hasfeature-modules-in-precompile-of-plain  $L/bin/clang++ "${BASE[@]}" -std=c++23 -fsyntax-only hf.cpp
printf 'export module m;\n#if __has_feature(modules)\n#error MODULES_ON\n#endif\n' > hf.cppm
try hasfeature-modules-in-module-interface $L/bin/clang++ "${BASE[@]}" -std=c++23 --precompile hf.cppm -o hf.pcm
try c++23            $L/bin/clang++ "${BASE[@]}" -std=c++23   --precompile $S -o a.pcm
try gnu++23          $L/bin/clang++ "${BASE[@]}" -std=gnu++23 --precompile $S -o b.pcm
try c++23-U-strict   $L/bin/clang++ "${BASE[@]}" -std=c++23 -U__STRICT_ANSI__ --precompile $S -o c.pcm
try c++23-inc-math   $L/bin/clang++ "${BASE[@]}" -std=c++23 -include math.h --precompile $S -o d.pcm
try c++23-fbuiltin-inf $L/bin/clang++ "${BASE[@]}" -std=c++23 "-DINFINITY=__builtin_inff()" "-DNAN=__builtin_nanf(\"\")" --precompile $S -o e.pcm
try c++26            $L/bin/clang++ "${BASE[@]}" -std=c++26   --precompile $S -o f.pcm
SDK26=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
BASE26=(--no-default-config -nostdinc++ -isystem $L/include/c++/v1 -isysroot $SDK26 -Wno-reserved-module-identifier)
try c++23-sdk26.5    $L/bin/clang++ "${BASE26[@]}" -std=c++23 --precompile $S -o g.pcm
# a program importing std, end to end, with the variant that works
for std in c++23 gnu++23; do
  rm -rf p; mkdir p; cd p
  $L/bin/clang++ "${BASE[@]}" -std=$std --precompile $S -o std.pcm >/dev/null 2>&1
  $L/bin/clang++ "${BASE[@]}" -std=$std -c std.pcm -o std.o >/dev/null 2>&1
  printf 'import std;\nint main(){ std::println("inf={} nan={}", std::numeric_limits<double>::infinity(), std::isnan(std::nan(""))); }\n' > m.cpp
  $L/bin/clang++ "${BASE[@]}" -std=$std -fmodule-file=std=std.pcm -c m.cpp -o m.o >/dev/null 2>&1
  $L/bin/clang++ --no-default-config -isysroot $SDK -fuse-ld=lld -nostdlib++ m.o std.o $L/lib/libc++.a $L/lib/libc++abi.a -o m >/dev/null 2>&1
  r "program $std: $(./m 2>&1)"
  cd ..
done
exit 0
