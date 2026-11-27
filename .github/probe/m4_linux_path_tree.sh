#!/usr/bin/env bash
# M4 (Linux): the runner's own compiler tree named by path, with the released
# mcpp. Which builds succeed, and against which C library each program links.
. "$(dirname "$0")/common.sh"
export MCPP_HOME="$PROBE_TMP/mcpp-home"
fetch_mcpp
say "the runner's tree"
show g++ --version
show ls -la /usr/bin/g++ /usr/bin/gcc /usr/bin/ar /usr/bin/ld /usr/bin/clang++
show ldd --version

W="$PROBE_TMP/m4l"; rm -rf "$W"; mkdir -p "$W"
# $1 = name, $2 = toolchain table, $3 = with build program (yes|no)
variant() {
    local name="$1" table="$2" bp="$3" d="$W/$1"
    hello_project "$d"
    printf '\n[toolchain]\nlinux = %s\n' "$table" >> "$d/mcpp.toml"
    if [ "$bp" = yes ]; then
        printf '#include <cstdio>\n#include <linux/errno.h>\n#if defined(__clang__)\n#error build program compiled by clang\n#endif\nint main() { std::puts("build program ran"); return 0; }\n' > "$d/build.mcpp"
    fi
    say "M4-linux $name: linux = $table ; build program: $bp"
    ( cd "$d" && "$MCPP" build -v > "$d/build.log" 2>&1; echo "[exit $?]" >> "$d/build.log" )
    grep -nE '^error|error:|warning:|Resolved|Using|Target |compiler |c-abi|c\+\+-abi|build\.mcpp' "$d/build.log" | head -24 | cut -c1-400
    tail -n 6 "$d/build.log" | cut -c1-500
    local bin; bin=$(find "$d/target" -path '*/bin/hello' -type f 2>/dev/null | head -1)
    if [ -n "$bin" ]; then
        echo "-- the program: interpreter and needed libraries"
        readelf -l "$bin" | grep -i 'interpreter'
        readelf -d "$bin" | grep -E 'NEEDED|RPATH|RUNPATH' | cut -c1-300
        "$bin"; echo "[run exit $?]"
    fi
    local n; n=$(find "$d/target/.build-mcpp" -name build.ninja 2>/dev/null | head -1)
    if [ -n "$n" ]; then
        echo "-- the build programs' sub-build: its tools"
        grep -nE '^(cxx|cc|ar) ' "$n" | cut -c1-200
        cat "$(dirname "$(dirname "$(dirname "$(dirname "$n")")")")/.mcpp-build-programs" 2>/dev/null
    fi
}
variant a-path-only        '{ path = "/usr" }'                                   no
variant b-family           '{ path = "/usr", family = "gcc" }'                    no
variant c-family-sysroot   '{ path = "/usr", family = "gcc", sysroot = "/" }'     no
variant d-bp-family        '{ path = "/usr", family = "gcc" }'                    yes
variant e-bp-sysroot       '{ path = "/usr", family = "gcc", sysroot = "/" }'     yes
say "M4-linux: full tails"
for v in a-path-only c-family-sysroot e-bp-sysroot; do echo "--- $v"; tail -n 40 "$W/$v/build.log" | cut -c1-700; done
