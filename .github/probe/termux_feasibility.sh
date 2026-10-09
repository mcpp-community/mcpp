#!/bin/sh
# Can llvm@23.1.3 + the managed glibc compile and run programs on Android, and
# does running the toolchain's loader environment only on the tool (not on the
# shell) avoid the "/bin/sh cannot link" failure? POSIX sh; prints only.
#   $1 = mcpp dist directory (MCPP_HOME) with llvm@23.1.3 already installed
#   $2 = a writable work directory
D="$1"; W="$2"
mkdir -p "$W"; cd "$W" || exit 0
LLVM=$(ls -d "$D"/registry/data/xpkgs/xim-x-llvm/23.1.3 2>/dev/null)
echo "FEAS llvm: $LLVM"
TRIPLE=$(ls "$LLVM/lib" | grep -E 'unknown-linux-gnu$' | head -1)
LIBDIR="$LLVM/lib/$TRIPLE"
echo "FEAS lib dir: $LIBDIR"

echo "FEAS P1 clang++ --version:"
"$LLVM/bin/clang++" --version 2>&1 | head -2 | sed 's/^/FEAS   /'

printf '#include <cstdio>\n#include <vector>\n#include <thread>\nint main(){ std::vector<int> v{1,2,3}; int s=0; std::thread t([&]{ for(int x: v) s+=x; }); t.join(); std::printf("glibc-llvm program ran, sum=%%d\\n", s); }\n' > h.cpp
echo "FEAS P2 compile + link + run with the payload's own configuration:"
"$LLVM/bin/clang++" -std=c++23 h.cpp -o h 2>&1 | head -10 | sed 's/^/FEAS   /'
if [ -x ./h ]; then ./h 2>&1 | sed 's/^/FEAS   /'; echo "FEAS   exit $?"; else echo "FEAS   no binary"; fi
echo "FEAS   interp: $(grep -a -o '/[^ ]*ld-linux[^ ]*\.so\.[0-9]*' ./h 2>/dev/null | head -1)"

echo "FEAS P3 the loader environment on the shell (what ninja does today):"
LD_LIBRARY_PATH="$LIBDIR" /system/bin/sh -c 'echo shell-started' 2>&1 | head -2 | sed 's/^/FEAS   /'
echo "FEAS P4 the loader environment on the tool only (D29):"
/system/bin/sh -c "env LD_LIBRARY_PATH='$LIBDIR' '$LLVM/bin/clang++' --version" 2>&1 | head -1 | sed 's/^/FEAS   /'

echo "FEAS P5 mcpp build with llvm@23.1.3 (today's engine):"
mkdir -p "$W/proj/src"; cd "$W/proj"
printf '[package]\nname = "feas"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "llvm@23.1.3"\n\n[targets.feas]\nkind = "bin"\nmain = "src/main.cpp"\n' > mcpp.toml
printf 'import std;\nint main() { std::println("import std on android"); }\n' > src/main.cpp
MCPP_HOME="$D" timeout 1200 "$D/bin/mcpp" build > build.log 2>&1
echo "FEAS   exit $?"
grep -E 'error|CANNOT|Finished|FAILED' build.log | head -6 | sed 's/^/FEAS   /'
exit 0
