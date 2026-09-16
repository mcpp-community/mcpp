#!/usr/bin/env bash
# macOS 27: why libc++ 22's std.cppm loses INFINITY/NAN. One READING per fact.
set +e
r() { echo "READING $*"; }
r "os $(sw_vers -productVersion) $(sw_vers -buildVersion) xcode=$(xcodebuild -version | tr '\n' ' ')"
r "xcrun sdk=$(xcrun --show-sdk-path) ver=$(xcrun --show-sdk-version)"
CLT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
r "clt sdk link=$(readlink $CLT) exists=$([ -d $CLT ] && echo y || echo n) settings=$(plutil -extract Version raw $CLT/SDKSettings.plist 2>/dev/null)"
ls -la /Library/Developer/CommandLineTools/SDKs/ 2>&1 | sed 's/^/READING clt-sdks /'
XSDK=$(xcrun --show-sdk-path)
for s in $CLT $XSDK; do
  r "math.h in $s: INFINITY defs: $(grep -rn 'define[[:space:]]*INFINITY' $s/usr/include/math.h $s/usr/include/_math.h $s/usr/include/math 2>/dev/null | head -3 | tr '\n' ' ')"
  r "math.h head in $s: $(grep -n 'include\|__MATH' $s/usr/include/math.h 2>/dev/null | head -12 | tr '\n' ' ')"
done
# xlings llvm
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash -s v2026.9.16.1 >/dev/null 2>&1
export PATH="$HOME/.xlings/subos/current/bin:$PATH"
xlings install llvm@22.1.8 -y >/dev/null 2>&1
L=$(ls -d $HOME/.xlings/data/xpkgs/xim-x-llvm/22.1.8)
r "llvm cfg: $(cat $L/bin/clang++.cfg | tr '\n' ' ')"
S=$L/share/libc++/v1/std.cppm
W=$(mktemp -d); cd $W
try() { name=$1; shift; out=$("$@" 2>&1); rc=$?; r "$name rc=$rc $(printf '%s' "$out" | grep -m2 'error' | tr '\n' ' ')"; }
try cfg-clt      $L/bin/clang++ -std=c++23 -Wno-reserved-module-identifier --precompile $S -o a.pcm
try isysroot-xc  $L/bin/clang++ -std=c++23 -Wno-reserved-module-identifier --precompile $S -o b.pcm --sysroot=$XSDK
try no-cfg-xc    $L/bin/clang++ --no-default-config -std=c++23 -nostdinc++ -isystem $L/include/c++/v1 -isysroot $XSDK -Wno-reserved-module-identifier --precompile $S -o c.pcm
printf '#include <cmath>\n#ifndef INFINITY\n#error no INFINITY after cmath\n#endif\n#include <complex>\nint main(){}\n' > t.cpp
try hdr-clt      $L/bin/clang++ -std=c++23 -fsyntax-only t.cpp
try hdr-xc       $L/bin/clang++ -std=c++23 -fsyntax-only t.cpp --sysroot=$XSDK
printf '#include <math.h>\n#ifndef INFINITY\n#error no INFINITY after math.h\n#endif\nint main(){}\n' > m.c
try c-math-clt   $L/bin/clang -fsyntax-only m.c
try c-math-apple /usr/bin/clang -fsyntax-only m.c
try cxx-apple    /usr/bin/clang++ -std=c++23 -fsyntax-only t.cpp
$L/bin/clang++ -std=c++23 -E -dD t.cpp 2>/dev/null | grep -n 'INFINITY\|# 1 ".*math' | head -10 | sed 's/^/READING pp-clt /'
$L/bin/clang++ -std=c++23 -H -fsyntax-only t.cpp 2>&1 | grep -i 'math' | head -12 | sed 's/^/READING includes-clt /'
# the released mcpp
xlings install mcpp@2026.9.16.2 -y >/dev/null 2>&1
M=$(ls $HOME/.xlings/data/xpkgs/xim-x-mcpp/2026.9.16.2/bin/mcpp)
cd $(mktemp -d); "$M" new h >/dev/null 2>&1; cd h
out=$("$M" run 2>&1); rc=$?
r "mcpp-2026.9.16.2 new+run rc=$rc $(printf '%s' "$out" | grep -m3 -E 'error|Hello|sysroot' | tr '\n' ' ')"
exit 0
