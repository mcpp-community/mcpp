#!/usr/bin/env bash
# Reproductions for .agents/docs/2026-10-26-control-contract-runtime-and-ide-ecosystem-design.md
#
# Scenarios (each prints and never asserts; run one with SCENARIO=<letter>):
#   A  #839  concurrent cold builds of one configuration (E4) and of two configurations (E5)
#   B  #838  split exception ABI without sycl (E1), and the link-order fix (E2)
#   C        query/build latency baseline (E8) -- needs hyperfine
#   D        xlings interface fixed wait (E9)  -- needs strace
#   E        cache list cost (E10)              -- needs strace
#   F        clang + libstdc++ import std, cross-.so catch (E14, E15)
#   G        mcpp run exit status (E16)
#   H  X2    unrecorded projection entry refused (E12, E13) -- FRESH=1 builds a fresh home (downloads gcc twice)
#   I        SYCL units vs the runtime collision set and foreign typeinfo (E19, E20) -- needs xim:dpcpp 7.1.0;
#            TENSORVIA=<checkout> adds Tensorvia's device units
#
# Uses only payloads already in the mcpp home, except H with FRESH=1.
set -u
M=${MCPP:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/2026.10.24.1/bin/mcpp}
R=${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs
W=${WORK:-/tmp/mcpp-ecosystem-repro}
export MCPP_JOBS=${MCPP_JOBS:-8}
mkdir -p "$W"
want() { [ -z "${SCENARIO:-}" ] || [ "$SCENARIO" = "$1" ]; }

modules_project() {   # $1 = dir
    rm -rf "$1"; mkdir -p "$1/src"
    printf '[package]\nname = "lk"\nversion = "0.1.0"\nstandard = "c++23"\n[toolchain]\ndefault = "gcc@16.1.0"\n' > "$1/mcpp.toml"
    for i in $(seq 1 24); do
        printf 'export module m%d;\nimport std;\nexport int f%d(){ std::vector<int> v(%d); return (int)v.size(); }\n' $i $i $i > "$1/src/m$i.cppm"
    done
    { echo 'import std;'; for i in $(seq 1 24); do echo "import m$i;"; done
      echo 'int main(){ std::println("{}", f1()+f24()); }'; } > "$1/src/main.cpp"
}

if want A; then
    echo "===== A: concurrent builds (#839)"
    modules_project "$W/a"; cd "$W/a"
    for r in 1 2 3 4 5; do
        rm -rf target
        ( nice -n 10 "$M" build >"$W/a1.log" 2>&1; echo $? >"$W/a1.rc" ) &
        ( sleep 0.$r; nice -n 10 "$M" build >"$W/a2.log" 2>&1; echo $? >"$W/a2.rc" ) &
        wait
        echo "round $r: rc=$(cat "$W/a1.rc"),$(cat "$W/a2.rc")  $(grep -h -m1 'error:' "$W/a1.log" "$W/a2.log" | head -1)"
    done
    ( "$M" build >/dev/null 2>&1; echo "dev rc=$?" ) & ( "$M" build --release >/dev/null 2>&1; echo "release rc=$?" ) & wait
fi

if want B; then
    echo "===== B: split exception ABI (#838)"
    G=$R/xim-x-gcc/16.1.0/lib64
    rm -rf "$W/b"; mkdir -p "$W/b/src" "$W/b/tests"; cd "$W/b"
    cat > mcpp.toml <<EOF
[package]
name     = "xrt"
version  = "0.1.0"
standard = "c++23"
[toolchain]
default = "llvm@22.1.8"
[lib]
path = "src/xrt.cppm"
[targets.xrt]
kind = "shared"
[build]
ldflags = ["-L$G", "-l:libstdc++.so.6"]
EOF
    printf 'export module xrt;\nimport std;\nexport namespace xrt { int checked(int v) { if (v < 0) throw std::runtime_error("negative"); return v * 2; } }\n' > src/xrt.cppm
    cat > tests/catch.cpp <<'EOF'
import std;
import xrt;
int main() {
    auto ep = std::make_exception_ptr(std::logic_error("x"));   // a libc++abi-only entry point
    try { xrt::checked(-1); }
    catch (const std::exception& e) { std::println("caught as std::exception: {}", e.what()); return 0; }
    catch (...)                     { std::println("std::exception missed -> (...)"); return 1; }
}
EOF
    nice -n 10 "$M" test 2>&1 | tail -3
    D=$(dirname "$(find target -name build.ninja | head -1)"); LL=$R/xim-x-llvm/22.1.8/bin
    "$LL/llvm-nm" -D --undefined-only "$D/bin/catch" | grep -E "__cxa_throw|__gxx_personality|_ZTISt13runtime_error"
    echo "-- relink with the toolchain's own runtime first (E2)"
    cd "$D"
    NJ=$(command -v ninja || find "$R" -name ninja -type f -perm -u+x | head -1)
    CMD=$("$NJ" -t commands bin/catch | tail -1)
    LIBS="$R/xim-x-llvm/22.1.8/lib/x86_64-unknown-linux-gnu/libc++.a $R/xim-x-llvm/22.1.8/lib/x86_64-unknown-linux-gnu/libc++abi.a"
    FIXED=$(echo "$CMD" | sed "s# -l:libstdc++.so.6# $LIBS -l:libstdc++.so.6#; s#-o bin/catch#-o bin/catch.fixed#; s#bin/catch.rsp#bin/catch.fixed.rsp#")
    printf 'obj/xrt.m.o\nobj/catch.o\nobj/std.o\n' > bin/catch.fixed.rsp
    eval "$FIXED" && ./bin/catch.fixed; echo "rc=$?  GLIBCXX imports: $("$LL/llvm-nm" -D --undefined-only bin/catch.fixed | grep -c GLIBCXX)"
fi

if want C; then
    echo "===== C: latency baseline"
    modules_project "$W/c"; cd "$W/c"; "$M" build >/dev/null 2>&1
    hyperfine -N --warmup 2 --runs 8 "$M --version" "$M --protocol-version" "$M build" "$M run" \
        "$M emit build-database --format json" "$M self env --format json" \
        "$M cache list --format json" "$M toolchain list --format json" 2>&1 | grep -E "^Benchmark|Time \(mean"
fi

if want D; then
    echo "===== D: xlings interface fixed wait"
    X=${MCPP_HOME:-$HOME/.mcpp}/registry/bin/xlings
    cd "${MCPP_HOME:-$HOME/.mcpp}/registry"
    for a in '--version' 'payload_status --args {"targets":["xim:gcc@16.1.0"]}'; do
        s=$(date +%s%N); env -u XLINGS_PROJECT_DIR -u XLINGS_ACTIVE_SUBOS XLINGS_HOME="$PWD" "$X" interface $a >/dev/null 2>&1
        echo "interface ${a%% *}: $(( ($(date +%s%N)-s)/1000000 )) ms"
    done
    strace -f -tt -T -e trace=nanosleep,clock_nanosleep,futex,poll -o "$W/d.txt" \
        env -u XLINGS_PROJECT_DIR -u XLINGS_ACTIVE_SUBOS XLINGS_HOME="$PWD" "$X" interface payload_status --args '{"targets":["xim:gcc@16.1.0"]}' >/dev/null 2>&1
    awk '{ if (match($0, /<([0-9.]+)>$/, m) && m[1] > 0.05) print }' "$W/d.txt"
fi

if want E; then
    echo "===== E: cache list"
    C=${MCPP_HOME:-$HOME/.mcpp}/build-cache
    echo "files: $(find "$C" -type f | wc -l)"
    strace -c -f -o "$W/e.txt" "$M" cache list --format json >/dev/null 2>&1; head -8 "$W/e.txt"
fi

if want F; then
    echo "===== F: clang + libstdc++"
    CXX=$R/xim-x-llvm/22.1.8/bin/clang++; GLIBC=$R/xim-x-glibc/2.44.3; UAPI=$R/xim-x-linux-headers/5.11.1
    for V in 15.1.0 16.1.0; do
        G=$R/xim-x-gcc/$V; GID=$(ls -d "$G"/lib/gcc/x86_64-linux-gnu/* | head -1)
        rm -rf "$W/f-$V"; mkdir -p "$W/f-$V"; cd "$W/f-$V"
        INC="-nostdinc++ -isystem $G/include/c++/$V -isystem $G/include/c++/$V/x86_64-linux-gnu -isystem $G/include/c++/$V/backward -nostdlibinc -isystem $GLIBC/include -isystem $UAPI/include"
        C="--no-default-config -std=c++23 --gcc-install-dir=$GID $INC"
        L="--no-default-config -stdlib=libstdc++ --gcc-install-dir=$GID -fuse-ld=lld --rtlib=libgcc --unwindlib=libgcc -L$G/lib64 -Wl,-rpath,$G/lib64 -B$GLIBC/lib64 -L$GLIBC/lib64 -Wl,-rpath,$GLIBC/lib64 -Wl,--dynamic-linker=$GLIBC/lib64/ld-linux-x86-64.so.2"
        "$CXX" $C -Wno-reserved-module-identifier --precompile -x c++-module "$G/include/c++/$V/bits/std.cc" -o std.pcm 2>/dev/null; echo "[$V] std.pcm rc=$?"
        "$CXX" $C -c std.pcm -o std.o 2>/dev/null
        printf 'import std;\nint main(){ std::vector<int> v{3,1,2}; std::ranges::sort(v); std::println("{} {}", v, std::format("{:.2f}", 3.14159));\n try { throw std::runtime_error("boom"); } catch (const std::exception& e) { std::println("caught: {}", e.what()); }\n std::jthread t([]{ std::println("thread ok"); }); }\n' > main.cpp
        "$CXX" $C -fmodule-file=std=std.pcm -c main.cpp -o main.o && "$CXX" $L main.o std.o -o app && ./app; echo "[$V] run rc=$?"
        printf '#include <stdexcept>\nint checked(int v){ if (v<0) throw std::runtime_error("negative (from .so)"); return v*2; }\n' > lib.cpp
        printf 'import std;\nint checked(int);\nint main(){ try { checked(-1); } catch (const std::exception& e) { std::println("caught across .so: {}", e.what()); return 0; } catch (...) { return 1; } }\n' > use.cpp
        "$CXX" $C -fPIC -c lib.cpp -o lib.o && "$CXX" $L -shared lib.o -o libthrow.so \
            && "$CXX" $C -fmodule-file=std=std.pcm -c use.cpp -o use.o && "$CXX" $L use.o std.o -L. -lthrow -Wl,-rpath,"$PWD" -o use && ./use
        echo "[$V] cross-.so rc=$?"
    done
fi

if want G; then
    echo "===== G: mcpp run exit status"
    rm -rf "$W/g"; mkdir -p "$W/g/src"; cd "$W/g"
    printf '[package]\nname = "rc"\nversion = "0.1.0"\nstandard = "c++23"\n' > mcpp.toml
    printf 'import std;\nint main(int argc, char* argv[]) {\n std::string_view m = argc > 1 ? argv[1] : "ok";\n if (m == "exit") return std::atoi(argv[2]);\n if (m == "throw") throw std::runtime_error("boom");\n if (m == "abort") std::abort();\n if (m == "segv") std::raise(11);\n return 0; }\n' > src/main.cpp
    "$M" build >/dev/null 2>&1
    for a in "ok" "exit 3" "exit 255" "exit 256" "throw" "abort" "segv"; do
        "$M" run -- $a >/dev/null 2>&1; echo "mcpp run -- $a => $?"
    done
fi

if want H; then
    echo "===== H: X2 unrecorded projection entry"
    if [ "${FRESH:-0}" = 1 ]; then
        H=$W/h-home; rm -rf "$H" "$W/h1"; mkdir -p "$W/h1/src"
        printf '[package]\nname = "p1"\nversion = "0.1.0"\nstandard = "c++23"\n[toolchain]\ndefault = "gcc@16.1.0"\n[xlings.workspace]\n"xim:gcc" = "15.1.0"\n' > "$W/h1/mcpp.toml"
        printf 'import std;\nint main(){ std::println("p1"); }\n' > "$W/h1/src/main.cpp"
        (cd "$W/h1" && MCPP_HOME=$H nice -n 10 "$M" build 2>&1 | tail -2)
        X=$H/registry/bin/xlings; E="env -u XLINGS_PROJECT_DIR -u XLINGS_ACTIVE_SUBOS XLINGS_HOME=$H/registry"
        cd "$H/registry"
        $E "$X" use gcc 15.1.0 2>&1 | tail -1; $E "$X" use gcc 16.1.0 2>&1 | tail -1   # E12: both succeed
        cp subos/default/.xlings.json "$W/h-ledger.bak"
        python3 - "$H/registry/subos/default/.xlings.json" <<'EOF'
import json, sys
p = sys.argv[1]; d = json.load(open(p))
def strip(o):
    if isinstance(o, dict):
        for k in [k for k in o if k in ("libatomic.so", "libatomic.so.1")]: del o[k]
        for v in o.values(): strip(v)
    elif isinstance(o, list):
        for v in o: strip(v)
strip(d); json.dump(d, open(p, "w"), indent=2)
EOF
        $E "$X" use gcc 15.1.0 2>&1 | tail -1                                                # E13: refused
        cp "$W/h-ledger.bak" subos/default/.xlings.json
    else
        echo "set FRESH=1 to build a fresh home (downloads gcc 16.1.0 and 15.1.0)"
    fi
fi

if want I; then
    echo "===== I: SYCL units vs the collision set (E19, E20)"
    DX=$R/xim-x-dpcpp/7.1.0/bin/clang++; G=$R/xim-x-gcc/15.1.0; GID=$(ls -d "$G"/lib/gcc/x86_64-linux-gnu/* | head -1)
    GLIBC=$R/xim-x-glibc/2.44.3; UAPI=$(ls -d "$R"/xim-x-linux-headers/* | head -1); LL=$R/xim-x-llvm/22.1.8; NM=$LL/bin/llvm-nm
    P=$R/mcpp-x-plugins/0.19.1/mcpp-plugins-0.19.1/tests/sycl-consumer
    rm -rf "$W/i"; mkdir -p "$W/i"; cd "$W/i"
    "$NM" -D --defined-only "$R/xim-x-gcc/16.1.0/lib64/libstdc++.so.6" | awk '{print $3}' | sed 's/@.*//' | sort -u > gnu.txt
    "$NM" --defined-only -g "$LL/lib/x86_64-unknown-linux-gnu/libc++.a" "$LL/lib/x86_64-unknown-linux-gnu/libc++abi.a" 2>/dev/null \
        | awk 'NF==3 && $2 ~ /[TDRVWBui]/ {print $3}' | sort -u > llvm.txt
    comm -12 gnu.txt llvm.txt > collide.txt; echo "collision set: $(wc -l < collide.txt)"
    F="-fsycl -std=c++17 -O2 -fPIC --gcc-install-dir=$GID -isystem$GLIBC/include -isystem$UAPI/include"
    nice -n 10 "$DX" $F -I"$P/include" -x c++ -c "$P/src/kernels/saxpy.sycl" -o saxpy.o
    if [ -n "${TENSORVIA:-}" ]; then
        for f in runtime arithmetic matmul; do
            nice -n 10 "$DX" $F -I"$TENSORVIA/include" -I"$TENSORVIA/src/backend/sycl" -x c++ -c "$TENSORVIA/src/backend/sycl/device/$f.sycl" -o "tv_$f.o"
        done
    fi
    for o in *.o; do
        "$NM" -u "$o" | awk '{print $2}' | sort -u > u.txt
        echo "== $o: $(comm -12 u.txt collide.txt | wc -l) in collision set; layout-sensitive:" \
             "$(comm -12 u.txt collide.txt | c++filt | grep -c -E 'runtime_error|logic_error|out_of_range|invalid_argument|length_error|::what\(\)')"
        echo "   foreign typeinfo: $(grep '^_ZTI' u.txt | comm -23 - collide.txt | c++filt | tr '\n' ' ')"
    done
fi
