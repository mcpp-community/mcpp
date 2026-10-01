#!/usr/bin/env bash
# Sandbox verification of the release that implements
# .agents/docs/2026-10-01-pack-drive-and-selection-independent-compile-design.md
# (#751, #753), run against the PUBLISHED release inside an xlings sandbox:
#
#   B64=$(base64 -w0 .agents/docs/2026-10-01-pack-drive-and-selection-verify.sh)
#   xlings subos new v1002 2>/dev/null || true
#   xlings subos use v1002 --sandbox --cmd "echo $B64 | base64 -d > /tmp/v.sh && VER=<version> bash /tmp/v.sh"
#
# VER selects the release under test. Running it with VER=2026.10.1.1 is the
# control: every section marked CHANGE must fail there, and every other
# section must pass on both.
#
# Every probe directory is removed at the start of its section, because the
# sandbox's $HOME persists between runs of the same subos. A section that does
# not run is reported as SKIP and counted apart from a pass.
set -u
VER="${VER:?VER=<version under test>}"
W="${W:-$HOME/v1002}"
fails=0; passes=0; skips=0
pass() { echo "PASS  $1"; passes=$((passes+1)); }
fail() { echo "FAIL  $1"; [ -n "${2:-}" ] && [ -f "$2" ] && tail -20 "$2"; fails=$((fails+1)); }
skip() { echo "SKIP  $1"; skips=$((skips+1)); }

# 0. The release under test, from the published channel, with the CN mirror.
if [ -n "${MCPP_OVERRIDE:-}" ]; then
    MCPP="$MCPP_OVERRIDE"
else
    xlings config --mirror CN >/dev/null 2>&1 || true
    xlings update >/dev/null 2>&1 || true
    xlings install "mcpp@$VER" -y > /tmp/v1002-install.log 2>&1 || true
    MCPP="$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp"
fi
if [ ! -x "$MCPP" ]; then
    echo "FATAL: mcpp $VER is not installable from the index"; tail -20 /tmp/v1002-install.log; exit 2
fi
got=$("$MCPP" --version 2>&1 | head -1)
case "$got" in *"$VER"*) pass "0 installed: $got";; *) fail "0 version: $got";; esac
[ -n "${MCPP_OVERRIDE:-}" ] || "$MCPP" self config --mirror CN >/dev/null 2>&1 || true
mkdir -p "$W"

# compiles_after <log> <lines>: the compile steps `.ninja_log` records after
# its first <lines> lines.
compiles_after() {
    tail -n +"$(( $2 + 1 ))" "$1" | awk -F'\t' '$4 ~ /\.(o|obj)$/ { n++ } END { print n + 0 }'
}
# no_compile <label> <selection...>: the build of the selection compiles nothing.
no_compile() {
    local label="$1"; shift
    local log before n
    log=$(find target -name .ninja_log | head -1)
    before=$(wc -l < "$log")
    "$MCPP" build "$@" > nc.log 2>&1 || { fail "$label: build $* failed" nc.log; return 1; }
    n=$(compiles_after "$log" "$before")
    [ "$n" = 0 ] || { fail "$label: build $* compiled $n units" nc.log; return 1; }
    return 0
}
member() {   # <dir> <name> <kind> [<dependency path>]
    mkdir -p "$1/src"
    {
        printf '[package]\nname = "%s"\nversion = "0.1.0"\n' "$2"
        [ -n "${4:-}" ] && printf '\n[dependencies]\ncore = { path = "%s" }\n' "$4"
        printf '\n[targets.%s]\nkind = "%s"\n' "$2" "$3"
        [ "$3" = bin ] && printf 'main = "src/main.cpp"\n'
    } > "$1/mcpp.toml"
}

# 1. CHANGE (#751, GalTranslPP's shape): two members compile one file from
# outside their directories; after --workspace, no selection compiles again.
rm -rf "$W/s1"; mkdir -p "$W/s1/shared"; cd "$W/s1"
printf '[workspace]\nmembers = ["core", "app", "tool"]\n' > mcpp.toml
printf 'export module m;\nexport int answer() { return 42; }\n' > shared/m.cppm
member core core lib; member app app bin ../core; member tool tool bin
printf '\n[build]\nsources = ["src/*.cppm", "../shared/m.cppm"]\n' >> core/mcpp.toml
printf '\n[build]\nsources = ["src/*.cpp", "../shared/m.cppm"]\n' >> tool/mcpp.toml
printf 'export module corelib;\nimport m;\nexport int core_value() { return answer(); }\n' > core/src/core.cppm
printf '#include <cstdio>\nimport corelib;\nint main() { std::printf("%%d\\n", core_value()); }\n' > app/src/main.cpp
printf '#include <cstdio>\nimport m;\nint main() { std::printf("%%d\\n", answer() + 1); }\n' > tool/src/main.cpp
if "$MCPP" build --workspace > s1.log 2>&1; then
    if no_compile "1" -p app && no_compile "1" -p tool && no_compile "1" --workspace && no_compile "1" -p app; then
        pass "1 CHANGE: a file two members list is compiled once for every selection"
    fi
else fail "1 the workspace did not build" s1.log; fi

# 2. CHANGE: two members each with their own module `m`; alternating
# selections compile nothing once each was built, and each program prints its
# own value.
rm -rf "$W/s2"; mkdir -p "$W/s2"; cd "$W/s2"
printf '[workspace]\nmembers = ["core", "app", "tool"]\n' > mcpp.toml
member core core lib; member app app bin ../core; member tool tool bin
printf 'export module m;\nexport int answer() { return 42; }\n' > core/src/m.cppm
printf 'export module corelib;\nimport m;\nexport int core_value() { return answer(); }\n' > core/src/core.cppm
printf '#include <cstdio>\nimport corelib;\nint main() { std::printf("%%d\\n", core_value()); }\n' > app/src/main.cpp
printf 'export module m;\nexport int answer() { return 7; }\n' > tool/src/m.cppm
printf '#include <cstdio>\nimport m;\nint main() { std::printf("%%d\\n", answer()); }\n' > tool/src/main.cpp
if "$MCPP" build --workspace > s2.log 2>&1; then
    a=$(find target -path '*/bin/app/app' -type f | head -1); t=$(find target -path '*/bin/tool/tool' -type f | head -1)
    if [ "$("$a")" = 42 ] && [ "$("$t")" = 7 ] \
       && no_compile "2" -p app && no_compile "2" -p tool && no_compile "2" -p app && no_compile "2" --workspace; then
        pass "2 CHANGE: two modules of one name, no recompile across selections, each program its own value"
    fi
else fail "2 the workspace did not build" s2.log; fi

# 3. CHANGE: a member that builds a shared library does not change the other
# members' commands.
rm -rf "$W/s3"; mkdir -p "$W/s3"; cd "$W/s3"
printf '[workspace]\nmembers = ["core", "app", "dso"]\n' > mcpp.toml
member core core lib; member app app bin ../core; member dso dso shared
printf 'export module corelib;\nexport int core_value() { return 3; }\n' > core/src/core.cppm
printf 'import corelib;\nint main() { return core_value() == 3 ? 0 : 1; }\n' > app/src/main.cpp
printf 'export module dsolib;\nexport int dso_value() { return 5; }\n' > dso/src/dso.cppm
if "$MCPP" build --workspace > s3.log 2>&1; then
    if no_compile "3" -p app && no_compile "3" --workspace; then
        pass "3 CHANGE: a shared-library member leaves the other members' commands alone"
    fi
else fail "3 the workspace did not build" s3.log; fi

# 4. CHANGE (#753): a pack states its build, and Finished precedes Packing.
rm -rf "$W/s4"; mkdir -p "$W/s4"; cd "$W/s4"
member . p4 bin
printf 'int main() { return 0; }\n' > src/main.cpp
if "$MCPP" pack --format tar > s4.log 2>&1; then
    fin=$(grep -n 'Finished' s4.log | head -1 | cut -d: -f1); pk=$(grep -n 'Packing' s4.log | head -1 | cut -d: -f1)
    if grep -q 'Compiling p4' s4.log && [ -n "$fin" ] && [ -n "$pk" ] && [ "$fin" -lt "$pk" ]; then
        pass "4 CHANGE: the pack states its build, Finished before Packing"
    else fail "4 CHANGE: the pack's build is not stated" s4.log; fi
else fail "4 the pack failed" s4.log; fi

# 5. CHANGE: `[build] jobs = 1` bounds mcpp test's compiles.
rm -rf "$W/s5"; mkdir -p "$W/s5"; cd "$W/s5"
member . p5 bin
printf '\n[build]\njobs = 1\n' >> mcpp.toml
mkdir -p tests
for i in 1 2 3 4; do
    printf 'constexpr unsigned long long s%s() { unsigned long long x = %s; for (unsigned k = 0; k < 600; ++k) for (unsigned j = 0; j < 600; ++j) x = x * 6364136223846793005ull + 1; return x; }\nunsigned long long v%s() { constexpr auto v = s%s(); return v; }\n' $i $i $i $i > src/u$i.cpp
done
printf 'int main() { return 0; }\n' > src/main.cpp
printf 'int main() { return 0; }\n' > tests/t.cpp
if "$MCPP" test > s5.log 2>&1; then
    log=$(find target -name .ninja_log | head -1)
    if awk -F'\t' 'NR > 1 && $4 ~ /\.o$/ { s[n] = $1; e[n] = $2; n++ }
                   END { for (i = 0; i < n; i++) for (j = i + 1; j < n; j++) if (s[i] < e[j] && s[j] < e[i]) exit 1; exit (n < 4) }' "$log"; then
        pass "5 CHANGE: mcpp test runs one compile at a time under jobs = 1"
    else fail "5 CHANGE: mcpp test overlapped compiles under jobs = 1" "$log"; fi
else fail "5 mcpp test failed" s5.log; fi

# 6. Index packages that provide modules build, run, and are served from the
# global cache by a second project, their BMIs below their packages' directories.
for d in s6a s6b; do
    rm -rf "$W/$d"; mkdir -p "$W/$d/src"
    printf '[package]\nname = "eco"\nversion = "0.1.0"\n\n[dependencies]\n"compat.zlib" = "*"\n"mcpplibs.cmdline" = "*"\n\n[targets.eco]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$W/$d/mcpp.toml"
    cat > "$W/$d/src/main.cpp" <<'EOF'
#include <cstdio>
#include <zlib.h>
import mcpplibs.cmdline;
int main() { std::printf("zlib %s\n", zlibVersion()); return 0; }
EOF
done
cd "$W/s6a"
if "$MCPP" run > s6a.log 2>&1 && grep -q '^zlib ' s6a.log; then
    cd "$W/s6b"
    if "$MCPP" run > s6b.log 2>&1 && grep -q '^zlib ' s6b.log && grep -q 'Cached mcpplibs.cmdline' s6b.log; then
        pass "6a index packages build, run, and are served from the cache"
        if [ -n "$(find target -path '*.cache/*cmdline*/*' -type f | head -1)" ]; then
            pass "6b CHANGE: a cached dependency's BMIs are staged below its package's directory"
        else fail "6b CHANGE: the cached BMIs are staged at their names" s6b.log; fi
    else fail "6a the second project was not served from the cache" s6b.log; fi
else fail "6 index packages" s6a.log; fi

# 7. xlings, built from its source with the release under test.
rm -rf "$W/s7"; mkdir -p "$W/s7"; cd "$W/s7"
if git clone -q --depth 1 https://github.com/openxlings/xlings.git xlings > s7-clone.log 2>&1 \
   || git clone -q --depth 1 https://gitee.com/openxlings/xlings.git xlings > s7-clone.log 2>&1; then
    cd xlings
    if "$MCPP" build > s7.log 2>&1; then
        x=$(find target -path '*/bin/xlings' -type f | head -1)
        if [ -n "$x" ] && "$x" --version > s7v.log 2>&1; then
            if no_compile "7" ; then pass "7 xlings builds with the release, runs, and a second build compiles nothing: $(head -1 s7v.log)"; fi
        else fail "7 the xlings that was built does not run" s7v.log; fi
    else fail "7 xlings does not build" s7.log; fi
else skip "7 xlings could not be cloned"; fi

echo "---- $passes passed, $fails failed, $skips skipped"
[ "$fails" = 0 ]
