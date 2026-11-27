#!/usr/bin/env bash
# M4: the macOS runner's own compiler tree named by path, with the released
# mcpp. Which of a program, an archive, a build program and a module unit build.
. "$(dirname "$0")/common.sh"
export MCPP_HOME="$PROBE_TMP/mcpp-home"
fetch_mcpp
say "the runner's tree"
show sw_vers
show xcodebuild -version
show clang++ --version
XCT="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr"
echo "XCT=$XCT"
show ls "$XCT/bin"
show sh -c 'command -v ld64.lld; command -v llvm-ar; command -v clang-scan-deps'
show sh -c 'ar --version 2>&1 | head -2'
echo "-- does the runner's ar take a response file"
( cd "$PROBE_TMP" && printf 'int f(void){return 0;}\n' > f.c && clang -c f.c -o f.o && printf 'f.o\n' > f.rsp \
  && ar rcs libf.a @f.rsp; echo "[ar @rsp exit $?]"; ar t libf.a 2>&1 | head -3 )
echo "-- does the runner's clang++ take module syntax as it is"
( cd "$PROBE_TMP" && printf 'export module m;\nexport int f() { return 1; }\n' > m.cppm \
  && clang++ -std=c++23 --precompile m.cppm -o m.pcm; echo "[precompile exit $?]" \
  ; clang++ -std=c++23 -fcxx-modules --precompile m.cppm -o m2.pcm; echo "[precompile -fcxx-modules exit $?]" )
echo "-- does the runner's clang++ link with -fuse-ld=lld"
( cd "$PROBE_TMP" && printf 'int main(){return 0;}\n' > l.cpp && clang++ l.cpp -fuse-ld=lld -o l 2>&1 | head -3; echo "[exit ${PIPESTATUS[0]}]" )
echo "-- compiler-rt and libunwind as the driver reports them"
show clang++ --rtlib=compiler-rt -print-libgcc-file-name
show clang++ -print-file-name=libunwind.a
show clang++ -print-file-name=libunwind.dylib

W="$PROBE_TMP/m4"; rm -rf "$W"; mkdir -p "$W"
# $1 = name, $2 = toolchain table, $3 = shape (bin|lib|bp|module)
variant() {
    local name="$1" table="$2" shape="$3" d="$W/$1"
    hello_project "$d"
    printf '\n[toolchain]\nmacos = %s\n' "$table" >> "$d/mcpp.toml"
    case "$shape" in
    lib)
        printf '\n[targets.hellolib]\nkind = "lib"\n' >> "$d/mcpp.toml"
        printf 'int hello_value() { return 7; }\n' > "$d/src/value.cpp" ;;
    bp)
        printf '#include <cstdio>\nint main() { std::puts("build program ran"); return 0; }\n' > "$d/build.mcpp" ;;
    module)
        printf 'export module hello.m;\nexport int value() { return 7; }\n' > "$d/src/m.cppm"
        printf 'import hello.m;\n#include <cstdio>\nint main() { std::printf("%%d\\n", value()); return 0; }\n' > "$d/src/main.cpp" ;;
    esac
    say "M4 $name: macos = $table ; shape: $shape"
    ( cd "$d" && "$MCPP" build -v > "$d/build.log" 2>&1; echo "[exit $?]" >> "$d/build.log" )
    grep -nE '^error|error:|warning:|Resolved|Using|compiler |c\+\+-abi|build\.mcpp|ld64|fuse-ld|\.rsp' "$d/build.log" | head -20 | cut -c1-500
    tail -n 5 "$d/build.log" | cut -c1-600
    local bin; bin=$(find "$d/target" -path '*/bin/hello' -type f 2>/dev/null | head -1)
    if [ -n "$bin" ]; then "$bin"; echo "[run exit $?]"; otool -L "$bin" | head -6; fi
}
USRT='{ path = "/usr", family = "llvm" }'
USRLD='{ path = "/usr", family = "llvm", tools = { ld = "/usr/bin/ld" } }'
XCTT="{ path = \"$XCT\", family = \"llvm\" }"
XCTLD="{ path = \"$XCT\", family = \"llvm\", tools = { ld = \"$XCT/bin/ld\" } }"
variant a-usr-bin        "$USRT"  bin
variant b-usr-ld-bin     "$USRLD" bin
variant c-usr-ld-lib     "$USRLD" lib
variant d-usr-ld-bp      "$USRLD" bp
variant e-usr-ld-module  "$USRLD" module
variant f-xct-bin        "$XCTT"  bin
variant g-xct-ld-bin     "$XCTLD" bin
variant h-xct-ld-lib     "$XCTLD" lib
variant i-xct-ld-bp      "$XCTLD" bp
say "M4: full tails"
for v in a-usr-bin b-usr-ld-bin c-usr-ld-lib d-usr-ld-bp e-usr-ld-module g-xct-ld-bin; do
    echo "--- $v"; tail -n 45 "$W/$v/build.log" | cut -c1-800
done
