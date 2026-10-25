#!/usr/bin/env bash
# O8: clang (llvm payload) + libstdc++ (gcc payload) on this host's
# architecture: `import std`, `import std.compat`, a cross-.so typed catch, and
# sanitizers. Prints; never asserts.
set -u
R=${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs
ARCH=$(uname -m)
CXX=$R/xim-x-llvm/22.1.8/bin/clang++
V=16.1.0; G=$R/xim-x-gcc/$V
GID=$(ls -d "$G"/lib/gcc/*-linux-gnu/* | head -1)
TRIPLE=$(basename "$(dirname "$GID")")
GLIBC=$(ls -d "$R"/xim-x-glibc/* | tail -1)
UAPI=$(ls -d "$R"/xim-x-linux-headers/* | tail -1)
LOADER=$(ls "$GLIBC"/lib64/ld-linux-*.so.* "$GLIBC"/lib/ld-linux-*.so.* 2>/dev/null | head -1)
LIBDIR=$(dirname "$LOADER")
echo "arch=$ARCH triple=$TRIPLE gid=$GID glibc=$GLIBC loader=$LOADER"
ls "$G/include/c++/$V/bits/" | grep -E '^std(\.compat)?\.cc$'
W=$(mktemp -d); cd "$W"
INC="-nostdinc++ -isystem $G/include/c++/$V -isystem $G/include/c++/$V/$TRIPLE -isystem $G/include/c++/$V/backward -nostdlibinc -isystem $GLIBC/include -isystem $UAPI/include"
C="--no-default-config -std=c++23 --gcc-install-dir=$GID $INC"
L="--no-default-config -stdlib=libstdc++ --gcc-install-dir=$GID -fuse-ld=lld --rtlib=libgcc --unwindlib=libgcc -L$G/lib64 -L$G/lib -Wl,-rpath,$G/lib64:$G/lib -B$LIBDIR -L$LIBDIR -Wl,-rpath,$LIBDIR -Wl,--dynamic-linker=$LOADER"
step() { local label=$1; shift; "$@" >"$W/log" 2>&1; local rc=$?; echo "O8.$ARCH $label rc=$rc"; [ $rc -ne 0 ] && tail -5 "$W/log"; return $rc; }
step "std.pcm" "$CXX" $C -Wno-reserved-module-identifier --precompile -x c++-module "$G/include/c++/$V/bits/std.cc" -o std.pcm
step "std.compat.pcm" "$CXX" $C -Wno-reserved-module-identifier -fmodule-file=std=std.pcm --precompile -x c++-module "$G/include/c++/$V/bits/std.compat.cc" -o std.compat.pcm
"$CXX" $C -c std.pcm -o std.o; "$CXX" $C -fmodule-file=std=std.pcm -c std.compat.pcm -o std.compat.o
printf 'import std;\nimport std.compat;\nint main(){ std::vector<int> v{3,1,2}; std::ranges::sort(v); std::println("{} {}", v, ::abs(-4));\n try { throw std::runtime_error("boom"); } catch (const std::exception& e) { std::println("caught {}", e.what()); }\n std::jthread t([]{ std::println("thread ok"); }); }\n' > main.cpp
step "import std + std.compat build" "$CXX" $C -fmodule-file=std=std.pcm -fmodule-file=std.compat=std.compat.pcm -c main.cpp -o main.o
step "link" "$CXX" $L main.o std.o std.compat.o -o app && ./app; echo "O8.$ARCH run rc=$?"
printf '#include <stdexcept>\nint checked(int v){ if (v<0) throw std::runtime_error("negative (from .so)"); return v*2; }\n' > lib.cpp
printf 'import std;\nint checked(int);\nint main(){ try { checked(-1); } catch (const std::exception& e) { std::println("caught across .so: {}", e.what()); return 0; } catch (...) { return 1; } }\n' > use.cpp
"$CXX" $C -fPIC -c lib.cpp -o lib.o && "$CXX" $L -shared lib.o -o libthrow.so \
  && "$CXX" $C -fmodule-file=std=std.pcm -c use.cpp -o use.o && "$CXX" $L use.o std.o -L. -lthrow -Wl,-rpath,"$PWD" -o use && ./use
echo "O8.$ARCH cross-.so typed catch rc=$?"
for S in address undefined thread; do
  "$CXX" $C -fsanitize=$S -c std.pcm -o std-$S.o 2>/dev/null
  printf 'import std;\nint main(){ std::vector<int> v{1,2,3}; try { throw std::runtime_error("x"); } catch (const std::exception& e) { std::println("{} {}", e.what(), v.size()); } }\n' > s.cpp
  "$CXX" $C -fsanitize=$S -fmodule-file=std=std.pcm -c s.cpp -o s.o && "$CXX" $L -fsanitize=$S s.o std-$S.o -o s-$S >"$W/log" 2>&1 && ./s-$S
  rc=$?; echo "O8.$ARCH sanitizer=$S rc=$rc"; [ $rc -ne 0 ] && tail -3 "$W/log"
done
printf 'import std;\nint main(){ int* p = new int[2]; p[2]=1; std::println("{}", p[0]); delete[] p; }\n' > bad.cpp
"$CXX" $C -fsanitize=address -fmodule-file=std=std.pcm -c bad.cpp -o bad.o && "$CXX" $L -fsanitize=address bad.o std-address.o -o bad && ./bad 2>&1 | grep -m1 "ERROR: AddressSanitizer"
echo "O8.$ARCH asan detects overflow (grep rc=$?)"
readelf -d app | grep NEEDED
