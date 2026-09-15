#!/usr/bin/env bash
# Probes for .agents/docs/2026-09-16-646-649-four-issues-by-home.md.
#
# Four independent groups. Each is written out as a standalone script and run
# with its own options and variables. Every result is a line starting
# with READING, and the record quotes those lines.
#
#   a  #646 F1, F3 and the driver half of #649 E10 (needs gcc@16.1.0, llvm@22.1.8)
#   b  #647 E1 and #649 E5, E9 (the Android leg needs xim:android-ndk installed)
#   c  #647 E4 and #649 E6, E7, E8 (host only)
#   d  #648: the inherited pipe (A2), the refresh trigger (T), the mirror digests (A6)
#
# Usage: 2026-09-16-646-649-probes.sh [a|b|c|d|all] [work dir]
# Engine: the released mcpp 2026.9.15.2 (override with M=/path/to/mcpp); home ~/.mcpp.
# Group d's T reading plans a checkout of openxlings/xlings (XL=<path>).
set -u
WHICH=${1:-all}
WORK=${2:-$(mktemp -d)}
mkdir -p "$WORK" && WORK=$(cd "$WORK" && pwd)
export M=${M:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/2026.9.15.2/bin/mcpp}
export MCPP="$M"

group_a() { mkdir -p "$WORK/a"; cat > "$WORK/a.sh" <<'__PROBES_646_649_GROUP_A__'
# Group a: probes for #646 F1/F3 (Linux x86_64) and the driver half of #649 E10.
# Engine: the released mcpp 2026.9.15.2 (xlings package; home ~/.mcpp).
# Every result is a line starting with READING.
# Usage: probes.sh [work dir]  (default: ./work next to this script)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$HERE/work}
mkdir -p "$WORK" && WORK=$(cd "$WORK" && pwd)
M=${MCPP:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/2026.9.15.2/bin/mcpp}
NINJA=$(find "$HOME/.mcpp/registry/data/xpkgs/xim-x-ninja" -name ninja -type f | head -1)
LLVM=$HOME/.mcpp/registry/data/xpkgs/xim-x-llvm/22.1.8
GCCBIN=$HOME/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/bin
unset XLINGS_ACTIVE_SUBOS
reading() { echo "READING $*"; }
# Replay the engine's own link of bin/libfw.so from the build directory, with
# extra inputs ($1) and extra flags ($2). Ninja deletes response files after a
# link, so the file is rewritten from the edge's inputs, as probe M5 did.
relink_fw() {
    local cmd ins
    cmd=$("$NINJA" -t commands bin/libfw.so | tail -1)
    ins=$(grep '^build bin/libfw.so' build.ninja | sed 's/^build bin\/libfw.so : c[a-z_]* //; s/ || .*//; s/ | .*//')
    case "$cmd" in
        *libfw.so.rsp*) printf '%s %s\n' "$ins" "$1" > bin/libfw.so.rsp; eval "$cmd $2" ;;
        *) eval "${cmd/ -o / $1 -o } $2" ;;
    esac
}
"$M" --version | sed 's/^/READING engine: /'

# ---------------------------------------------------------------- F1 --------
# F1a: M3b verbatim (x static C, fw shared C over x, root C++ over fw).
f1_fixture() {   # $1 dir, $2 extra root deps line (may be empty), $3 main body
    mkdir -p "$1/app/src" "$1/fw/src" "$1/x/src"
    printf '[package]\nname = "x"\nversion = "0.1.0"\n[build]\nsources = ["src/*.c"]\n[targets.x]\nkind = "lib"\n' > "$1/x/mcpp.toml"
    printf 'int x_counter = 0;\nint x_answer(void) { return 41; }\nint x_bump(void) { return ++x_counter; }\n' > "$1/x/src/x.c"
    printf '[package]\nname = "fw"\nversion = "0.1.0"\n[build]\nsources = ["src/*.c"]\n[targets.fw]\nkind = "shared"\n[dependencies.x]\npath = "../x"\n' > "$1/fw/mcpp.toml"
    printf 'extern int x_answer(void);\nextern int x_bump(void);\nint fw_answer(void) { return x_answer() + 1; }\nint fw_bump(void) { return x_bump(); }\n' > "$1/fw/src/fw.c"
    printf '[package]\nname = "app"\nversion = "0.1.0"\n[toolchain]\ndefault = "gcc@16.1.0"\n[dependencies.fw]\npath = "../fw"\n%s' "$2" > "$1/app/mcpp.toml"
    printf '%s\n' "$3" > "$1/app/src/main.cpp"
}
f1_fixture "$WORK/f1a" "" 'extern "C" int fw_answer(void);
int main() { return fw_answer() == 42 ? 0 : 1; }'
cd "$WORK/f1a/app" && timeout 900 "$M" build > build.log 2>&1
reading "F1a M3b exit=$?"
d=$(ls -d target/*/*/ | head -1)
so=$d/bin/libfw.so; bin=$d/bin/app
reading "F1a libfw.so inputs: $(grep '^build bin/libfw.so' "$d/build.ninja" | sed 's/^build bin\/libfw.so : c[a-z_]* //; s/ |.*//')"
reading "F1a libfw.so undefined x_answer=$(nm -D --undefined-only "$so" | grep -c ' x_answer$') program defines x_answer=$(nm "$bin" | grep -c ' T x_answer$') program exports x_answer=$(nm -D --defined-only "$bin" | grep -c ' x_answer$')"
"$bin"; reading "F1a run exit=$?"

# F1c: the same library linked the way Mach-O and PE link (every reference
# resolved at link time): ELF's -z defs.
cd "$d"
relink_fw "" "-Wl,-z,defs" > f1c.log 2>&1
reading "F1c libfw.so relinked with -z defs: exit=$? $(grep -m1 -o 'undefined reference to .x_answer.\|undefined symbol: x_answer' f1c.log)"
relink_fw "" "" > /dev/null 2>&1   # restore

# F1d: a host that did not link x (a foreign program, or Android's
# System.loadLibrary("fw") before "app") loads the library with RTLD_NOW.
cat > "$WORK/f1a/host.c" <<'C'
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char** argv) {
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    printf("dlopen ok\n"); return 0;
}
C
"$GCCBIN/gcc" -o "$WORK/f1a/host" "$WORK/f1a/host.c" -ldl > /dev/null 2>&1
out=$("$WORK/f1a/host" "$PWD/bin/libfw.so" 2>&1); rc=$?
reading "F1d foreign host dlopen(libfw.so, RTLD_NOW): exit=$rc output: $out"

# F1b: x reachable from both the program and the shared image.
f1_fixture "$WORK/f1b" '[dependencies.x]
path = "../x"
' '#include <cstdio>
extern "C" int fw_answer(void); extern "C" int fw_bump(void);
extern "C" int x_bump(void); extern "C" int x_counter;
int main() { x_bump(); fw_bump(); std::printf("counter=%d\n", x_counter); return fw_answer() == 42 ? 0 : 1; }'
cd "$WORK/f1b/app" && timeout 900 "$M" build > build.log 2>&1
reading "F1b root also depends on x: exit=$?"
d=$(ls -d target/*/*/ | head -1)
reading "F1b x.o linked into: program=$(grep '^build bin/app ' "$d/build.ninja" | grep -c 'x\.c\.o\|/x\.o') libfw.so=$(grep '^build bin/libfw.so' "$d/build.ninja" | grep -c 'x\.c\.o\|/x\.o')"
out=$("$d/bin/app"); reading "F1b run exit=$? $out"

# F1f: what "the static package goes into the image" produces on ELF when the
# program links it as well: relink libfw.so with x's objects added.
cd "$d"
xo=$(grep -o '[^ ]*x\.c\.o\|[^ ]*/x\.o' build.ninja | head -1)
relink_fw "$xo" "" > f1f.log 2>&1
reading "F1f libfw.so relinked with x.o: exit=$? libfw defines x_counter=$(nm -D --defined-only bin/libfw.so | grep -c ' x_counter$') run: $(./bin/app) (1 shared counter => 2; split => 1)"

# F1e: the root itself is the shared image over x (root-owned unit).
mkdir -p "$WORK/f1e" && cp -r "$WORK/f1a/x" "$WORK/f1e/x" && mkdir -p "$WORK/f1e/fw/src"
printf '[package]\nname = "fw"\nversion = "0.1.0"\n[toolchain]\ndefault = "gcc@16.1.0"\n[build]\nsources = ["src/*.c"]\n[targets.fw]\nkind = "shared"\n[dependencies.x]\npath = "../x"\n' > "$WORK/f1e/fw/mcpp.toml"
cp "$WORK/f1a/fw/src/fw.c" "$WORK/f1e/fw/src/fw.c"
cd "$WORK/f1e/fw" && timeout 900 "$M" build > build.log 2>&1
so=$(ls target/*/*/bin/libfw.so | head -1)
reading "F1e root-owned shared image over x: exit=$? libfw.so defines x_answer=$(nm -D --defined-only "$so" | grep -c ' x_answer$') undefined=$(nm -D --undefined-only "$so" | grep -c ' x_answer$')"

# ---------------------------------------------------------------- F3 --------
F3_DEFAULT_MAIN='import std;
import lib;
int main() { std::println("{}", lib_greet(3)); }'
f3() {   # $1 id, $2 toolchain, [$3 root manifest tail], [$4 lib kind], [$5 main body]
    local w="$WORK/f3-$1"
    mkdir -p "$w/app/src" "$w/lib/src"
    printf '[package]\nname = "lib"\nversion = "0.1.0"\n[targets.lib]\nkind = "%s"\n' "${4:-shared}" > "$w/lib/mcpp.toml"
    printf 'export module lib;\nimport std;\nexport [[gnu::visibility("default")]] std::string lib_greet(int n);\n' > "$w/lib/src/lib.cppm"
    printf 'module lib;\nimport std;\nstd::string lib_greet(int n) { return std::format("lib-{}", n); }\n' > "$w/lib/src/lib.cpp"
    printf '[package]\nname = "app"\nversion = "0.1.0"\n[toolchain]\ndefault = "%s"\n[dependencies.lib]\npath = "../lib"\n%s' "$2" "${3:-}" > "$w/app/mcpp.toml"
    local body=${5:-$F3_DEFAULT_MAIN}
    printf '%s\n' "$body" > "$w/app/src/main.cpp"
    cd "$w/app"
    timeout 1800 "$M" build > build.log 2>&1; local rc=$?
    reading "F3 $1 build exit=$rc symbol-provision mentions=$(grep -c 'symbol-provision\|also provided by a library it loads' build.log)"
    local d; d=$(ls -d target/*/*/ 2>/dev/null | head -1)
    [ -n "$d" ] || return
    reading "F3 $1 run: $("$d/bin/app" 2>&1) exit=$?"
    reading "F3 $1 NEEDED program: $(readelf -d "$d/bin/app" | grep -o 'Shared library: \[[^]]*\]' | tr '\n' ' ')"
    [ -f "$d/bin/liblib.so" ] || { reading "F3 $1 no liblib.so (static form)"; return; }
    reading "F3 $1 NEEDED liblib.so: $(readelf -d "$d/bin/liblib.so" | grep -o 'Shared library: \[[^]]*\]' | tr '\n' ' ')"
    reading "F3 $1 contracts in resolution.json: $(python3 -c 'import json,sys,glob; d=json.load(open(sys.argv[1])); print(json.dumps(d["runtime"]["cxx_runtime_by_role"]))' "$d/resolution.json" 2>&1)"
    reading "F3 $1 liblib.so link inputs: $(grep '^build bin/liblib.so' "$d/build.ninja" | sed 's/^build bin\/liblib.so : c[a-z_]* //; s/ |.*//')"
    python3 - "$d/.mcpp-runtime-verdicts.json" "$d/obj/std.o" "$1" <<'PY'
import json, subprocess, sys
doc = json.load(open(sys.argv[1]))
def walk(o):
    if isinstance(o, dict):
        if 'conflicts' in o and 'path' in o: yield o
        for v in o.values(): yield from walk(v)
    elif isinstance(o, list):
        for v in o: yield from walk(v)
stdo = set()
try:
    out = subprocess.run(['nm', '--defined-only', '-g', sys.argv[2]], capture_output=True, text=True).stdout
    stdo = {l.split()[-1] for l in out.splitlines() if l.strip()}
    strong = sum(1 for l in out.splitlines() if l.split()[-2] in ('T','D','B','R'))
    print(f"READING F3 {sys.argv[3]} std.o global definitions={len(stdo)} strong={strong} sample={sorted(stdo)[:3]}")
except Exception as e:
    print(f"READING F3 {sys.argv[3]} std.o unreadable: {e}")
for e in walk(doc):
    c = e.get('conflicts') or []
    names = [x.get('symbol') for x in c]
    fromstd = sum(1 for n in names if n in stdo)
    by = sorted({p for x in c for p in x.get('also_provided_by', [])})
    print(f"READING F3 {sys.argv[3]} {e['path']} status={e.get('status')} exported={e.get('exported')} conflicts={len(c)} defined-in-std.o={fromstd} first={names[:4]} providers={by}")
PY
    timeout 600 "$M" build --strict > strict.log 2>&1
    reading "F3 $1 --strict exit=$? (fast path may skip the check: $(grep -c 'symbol-provision' strict.log) mentions)"
    touch src/main.cpp; timeout 600 "$M" build --strict > strict2.log 2>&1
    reading "F3 $1 --strict after touch exit=$? mentions=$(grep -c 'symbol-provision\|also provided by' strict2.log)"
}
f3 gcc  "gcc@16.1.0"
f3 llvm "llvm@22.1.8"
# Controls: which part of the default shape the findings and the abort belong to.
f3 llvm-static-lib "llvm@22.1.8" "" "lib"
f3 llvm-no-println "llvm@22.1.8" "" "shared" 'import lib;
#include <cstdio>
int main() { std::puts(lib_greet(3).c_str()); }'
f3 llvm-tc "llvm@22.1.8" '[build]
cxx_runtime = "toolchain-coupled"
'
f3 gcc-tc "gcc@16.1.0" '[build]
cxx_runtime = "toolchain-coupled"
'

# ---------------------------------------------------------------- E10 -------
# The GNU-style clang driver on the MSVC triple, with no runtime flag: which
# CRT does it select at compile and at link? (Driver logic is host-independent.)
cd "$WORK" && printf 'int main(){return 0;}\n' > e10.cpp
c=$("$LLVM/bin/clang++" --target=x86_64-pc-windows-msvc -### -c e10.cpp 2>&1 | tail -1)
reading "E10 compile, no flag: dependent-lib=$(echo "$c" | grep -o -- '--dependent-lib=[a-z]*' | tr '\n' ' ') defines=$(echo "$c" | grep -o -- '"-D" "_[A-Z]*"' | tr '\n' ' ')"
l=$("$LLVM/bin/clang++" --target=x86_64-pc-windows-msvc -fuse-ld=lld -### e10.o -o e10.exe 2>&1 | tail -1)
reading "E10 link, no flag: $(echo "$l" | grep -o -- '-defaultlib:[a-z]*' | tr '\n' ' ')"
for v in dll static; do
    c=$("$LLVM/bin/clang++" --target=x86_64-pc-windows-msvc -fms-runtime-lib=$v -### -c e10.cpp 2>&1 | tail -1)
    l=$("$LLVM/bin/clang++" --target=x86_64-pc-windows-msvc -fms-runtime-lib=$v -fuse-ld=lld -### e10.o -o e10.exe 2>&1 | tail -1)
    reading "E10 -fms-runtime-lib=$v: compile $(echo "$c" | grep -o -- '--dependent-lib=[a-z]*\|"-D" "_[A-Z]*"' | tr '\n' ' ') link $(echo "$l" | grep -o -- '-defaultlib:[a-z]*' | tr '\n' ' ')"
done
__PROBES_646_649_GROUP_A__
  bash "$WORK/a.sh" "$WORK/a" </dev/null
}

group_b() { mkdir -p "$WORK/b"; cat > "$WORK/b.sh" <<'__PROBES_646_649_GROUP_B__'
# Group b: probes for #647 E1 and #649 E5/E9, against the released mcpp 2026.9.15.2
# on a Linux x86_64 host. Each reading is printed as `READING <id>: ...`.
# Usage: bash probes.sh [workdir]   (the Android leg needs xim:android-ndk installed)
set -uo pipefail
M=${M:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/2026.9.15.2/bin/mcpp}
W=${1:-$(mktemp -d)}
mkdir -p "$W"; cd "$W"
reading() { echo "READING $*"; }
reading "version: $($M --version)"

# ── E1: app -> a -> b (path deps); b declares [package.metadata.demo] ──────
mkdir -p e1/app/src e1/a/src e1/b/src
cat > e1/b/mcpp.toml <<'EOF'
[package]
name      = "b"
namespace = "spike"
version   = "0.2.0"
standard  = "c++20"

[package.metadata.demo]
resources = "res"

[targets.b]
kind = "lib"

[build]
sources = ["src/*.cpp"]
EOF
echo 'int b_answer() { return 40; }' > e1/b/src/b.cpp
cat > e1/a/mcpp.toml <<'EOF'
[package]
name      = "a"
namespace = "spike"
version   = "0.1.0"
standard  = "c++20"

[dependencies]
spike.b = { path = "../b" }

[targets.a]
kind = "lib"

[build]
sources = ["src/*.cpp"]
EOF
echo 'int b_answer(); int a_answer() { return b_answer() + 2; }' > e1/a/src/a.cpp
cat > e1/app/mcpp.toml <<'EOF'
[package]
name      = "app"
version   = "0.1.0"
standard  = "c++20"

[dependencies]
spike.a = { path = "../a" }

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'int a_answer();\nint main() { return a_answer() == 42 ? 0 : 1; }\n' > e1/app/src/main.cpp
cat > e1/app/build.mcpp <<'EOF'
import std;
import mcpp;
extern char** environ;
int main() {
    std::string s = "SPIKE deps:";
    for (char** e = environ; *e; ++e) {
        std::string_view v(*e);
        if (v.starts_with("MCPP_DEP_")) { s += " "; s += v.substr(0, v.find('=')); }
    }
    mcpp::warning(s.c_str());
    std::string c = "SPIKE run " + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    mcpp::warning(c.c_str());
    return 0;
}
EOF
for p in a b; do cat > e1/$p/build.mcpp <<EOF
import std;
import mcpp;
int main() {
    std::string s = "SPIKE order $p " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    mcpp::warning(s.c_str());
    return 0;
}
EOF
done
cd e1/app
out=$($M build --strict 2>&1); rc=$?
reading "E1-strict-metadata-table: exit=$rc warnings=$(grep -c '^warning: \(schema\|\[package\]\)' <<<"$out")"
reading "E1-root-env: $(grep -o 'SPIKE deps:.*' <<<"$out")"
reading "E1-program-order: $(grep -o 'SPIKE order [ab]' <<<"$out" | tr '\n' ' ')"
f=$(find target -name resolution.json | head -1)
reading "E1-graph-record: $(python3 -c "import json;j=json.load(open('$f'));print([ (p['package']['canonical'], sorted(p.keys())) for p in j['graph']['packages']])")"
first=$(grep -o 'SPIKE run [0-9]*' <<<"$out")
sleep 1; sed -i 's/resources = "res"/resources = "res2"/' ../b/mcpp.toml
out=$($M build 2>&1)
reading "E1-metadata-edit: $(grep -o 'build.mcpp [a-z ()]*' <<<"$out" | tr '\n' ';') replayed=$([ "$(grep -o 'SPIKE run [0-9]*' <<<"$out")" = "$first" ] && echo yes || echo no)"
sed -i 's/^standard  = "c++20"/standard  = "c++20"\nbogus_key = 1/' mcpp.toml
out=$($M build --strict 2>&1); rc=$?
reading "E1-unknown-package-key-strict: exit=$rc mentions=$(grep -c bogus_key <<<"$out")"
cd "$W"

# ── E5 / E9: shared dependency, Android app row and Linux desktop row ────────
mkdir -p e5/dep/src e5/app/src e5/hostapp/src
cat > e5/dep/mcpp.toml <<'EOF'
[package]
name      = "dep"
namespace = "spike"
version   = "0.1.0"
standard  = "c++20"

[targets.dep]
kind = "shared"

[build]
sources = ["src/*.cpp"]
EOF
printf '#include <string>\n[[gnu::visibility("default")]] int dep_answer() { return static_cast<int>(std::to_string(42).size()) + 40; }\n' > e5/dep/src/dep.cpp
cat > e5/app/mcpp.toml <<'EOF'
[package]
name      = "app"
version   = "0.1.0"
standard  = "c++20"

[dependencies]
spike.dep = { path = "../dep" }

[targets.app]
kind = "app"
main = "src/main.cpp"

[target.x86_64-linux-android]
min_api_level = 23
EOF
printf '#include <string>\nint dep_answer();\n[[gnu::visibility("default")]] int app_entry() { return dep_answer() + static_cast<int>(std::to_string(1).size()); }\nint main() { return app_entry() == 43 ? 0 : 1; }\n' > e5/app/src/main.cpp
cat > e5/hostapp/mcpp.toml <<'EOF'
[package]
name      = "hostapp"
version   = "0.1.0"
standard  = "c++20"

[dependencies]
spike.dep = { path = "../dep" }

[targets.hostapp]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'int dep_answer();\nint main() { return dep_answer() == 42 ? 0 : 1; }\n' > e5/hostapp/src/main.cpp

sections() { echo "bytes=$(stat -c %s "$1") symtab=$(readelf -S -W "$1" | grep -c ' .symtab') debug=$(readelf -S -W "$1" | grep -c ' .debug_')"; }

cd e5/hostapp
line=$($M pack --format tar 2>&1 | grep -E 'Packing')
reading "E5-desktop-status: $line"
rm -rf x && mkdir x && tar -xzf target/dist/hostapp-0.1.0-x86_64-linux-gnu.tar.gz -C x
for f in x/*/bin/hostapp x/*/lib/libdep.so; do reading "E5-desktop: ${f#x/*/} $(sections "$f")"; done
for a in "--release" "--dev" "--message-format json"; do
  err=$($M pack $a --format tar 2>&1 >/dev/null | tail -1); $M pack $a --format tar >/dev/null 2>&1; rc=$?
  reading "E9-pack $a: exit=$rc [$err]"
done
reading "E9-human-lines-on-stdout: bytes=$($M pack --format tar 2>/dev/null | wc -c)"
reading "E9-run-precedence(--profile dev --release): $($M run --profile dev --release 2>&1 | grep -o 'Finished [a-z]*')"
reading "E9-build-precedence(--profile dev --release): $($M build --profile dev --release 2>&1 | grep -o 'Finished [a-z]*')"
cd "$W"

if ls -d "$HOME"/.mcpp/registry/data/xpkgs/xim-x-android-ndk/* >/dev/null 2>&1; then
  cd e5/app
  line=$(MCPP_OFFLINE=1 $M pack --target x86_64-linux-android --format tar 2>&1 | grep -E 'Packing')
  reading "E5-android-status: $line"
  rm -rf x && mkdir x && tar -xzf target/dist/app-0.1.0-x86_64-linux-android.tar.gz -C x
  for f in x/*/lib/*.so; do reading "E5-android: ${f#x/*/} $(sections "$f")"; done
  cd "$W"
else
  reading "E5-android: skipped (xim:android-ndk not installed)"
fi
command -v swiftc >/dev/null || reading "E2-host: no swiftc on this host; E2 needs macos-15"
__PROBES_646_649_GROUP_B__
  bash "$WORK/b.sh" "$WORK/b" </dev/null
}

group_c() { mkdir -p "$WORK/c"; cat > "$WORK/c.sh" <<'__PROBES_646_649_GROUP_C__'
# Group c: probes for #647 E4 and #649 E6/E7/E8, against the released mcpp 2026.9.15.2.
# Usage: probes.sh [e41|e42|e43|e6|e6b|e6c|e7|e8|all]
set +e
M=${M:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/2026.9.15.2/bin/mcpp}
ROOT=${ROOT:-$(cd "$(dirname "$0")" && pwd)/work}
reading() { echo "READING $1: $2"; }
pkg() { # dir ns name kind [extra toml]
  mkdir -p "$1/src"
  { echo '[package]'; echo "name      = \"$3\""; [ -n "$2" ] && echo "namespace = \"$2\"";
    echo 'version   = "0.1.0"'; echo 'standard  = "c++20"'; echo; echo "[targets.$3]"; echo "kind = \"$4\"";
    [ "$4" = bin ] && echo 'main = "src/main.cpp"'; [ "$4" = lib ] && { echo; echo '[build]'; echo 'sources = ["src/*.cpp"]'; }
    echo; printf '%s\n' "${5:-}"; } > "$1/mcpp.toml"
}
run() { # label dir args...
  local label=$1 dir=$2; shift 2
  out=$(cd "$dir" && timeout 900 "$M" "$@" 2>&1); rc=$?
  printf '%s\n' "$out" > "$ROOT/$label.log"
  echo "rc=$rc" >> "$ROOT/$label.log"
}
mkdir -p "$ROOT"
$M --version

e41() { # a forward along [build-dependencies] edges, two levels, host only
  local W=$ROOT/e41; rm -rf "$W"; mkdir -p "$W"
  pkg "$W/kt" spike kt bin; echo 'int main(){return 0;}' > "$W/kt/src/main.cpp"
  pkg "$W/leaf" spike leaf lib '[features]
kt = []

[feature-deps.kt]
spike.kt = { path = "../kt", tools = ["kt"] }'
  echo 'int leaf_answer(){return 1;}' > "$W/leaf/src/leaf.cpp"
  pkg "$W/rules" spike rules lib '[build-dependencies]
spike.leaf = { path = "../leaf" }

[features]
kotlin = ["spike.leaf/kt"]'
  echo 'int rules_answer(){return 1;}' > "$W/rules/src/rules.cpp"
  pkg "$W/fw" spike fw lib '[build-dependencies]
spike.rules = { path = "../rules" }

[features]
kotlin = ["spike.rules/kotlin"]'
  echo 'int fw_answer(){return 42;}' > "$W/fw/src/fw.cpp"
  pkg "$W/app" "" app bin '[dependencies]
spike.fw = { path = "../fw", features = ["kotlin"] }'
  printf 'int fw_answer();\nint main(){return fw_answer()==42?0:1;}\n' > "$W/app/src/main.cpp"
  run e41-build "$W/app" build
  reading E4.1-build "$(grep -c 'forwards to dependency' $ROOT/e41-build.log) forward warnings; tool kt built: $(grep -c 'Building host tool kt' $ROOT/e41-build.log); $(tail -1 $ROOT/e41-build.log)"
  grep 'forwards to dependency' $ROOT/e41-build.log | sed 's/^/  /'
  run e41-strict "$W/app" build --strict
  reading E4.1-strict "$(grep -m1 -o 'error: .*' $ROOT/e41-strict.log | cut -c1-120); $(tail -1 $ROOT/e41-strict.log)"
  sed -i 's/features = \["kotlin"\]/features = []/' "$W/app/mcpp.toml"; rm -rf "$W/app/target"
  run e41-off "$W/app" build
  reading E4.1-control-without-feature "tool kt built: $(grep -c 'Building host tool kt' $ROOT/e41-off.log); $(tail -1 $ROOT/e41-off.log)"
  rm -rf "$W"/*/target
}

e42() { # [feature-deps.<f>] adding tools to an unconditionally declared dependency
  local W=$ROOT/e42; rm -rf "$W"; mkdir -p "$W"
  pkg "$W/installer" spike installer bin; echo '#include <cstdio>
int main(){std::puts("installer ran");return 0;}' > "$W/installer/src/main.cpp"
  # the installer package also needs a library so that the unconditional edge means something
  pkg "$W/app" "" app bin '[dependencies]
spike.installer = { path = "../installer" }

[features]
installer = []

[feature-deps.installer]
spike.installer = { tools = ["installer"] }'
  echo 'int main(){return 0;}' > "$W/app/src/main.cpp"
  run e42-sourceless "$W/app" build --features installer
  reading E4.2-sourceless "$(grep -m1 -o 'error: .*' $ROOT/e42-sourceless.log | cut -c1-160); $(tail -1 $ROOT/e42-sourceless.log)"
  # restated with the same source: the documented intent, spelled as the parser requires
  sed -i 's|spike.installer = { tools = \["installer"\] }|spike.installer = { path = "../installer", tools = ["installer"] }|' "$W/app/mcpp.toml"
  cat > "$W/app/build.mcpp" <<'EOF'
import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("installer", "installer");
    const char* b = mcpp::dep_bin("spike.installer", "installer");
    mcpp::warning((std::string("SPIKE short=[") + (a ? a : "") + "] qualified=[" + (b ? b : "") + "]").c_str());
    return 0;
}
EOF
  run e42-restated "$W/app" build --features installer
  reading E4.2-restated "$(grep -o 'SPIKE.*' $ROOT/e42-restated.log | sed "s|$HOME|~|g" | cut -c1-200); $(tail -1 $ROOT/e42-restated.log)"
  rm -rf "$W/app/target"
  run e42-restated-off "$W/app" build
  reading E4.2-restated-without-feature "$(grep -o 'SPIKE.*' $ROOT/e42-restated-off.log); $(tail -1 $ROOT/e42-restated-off.log)"
  # a restatement naming a DIFFERENT source than the unconditional declaration
  mkdir -p "$W/other"; cp -r "$W/installer/." "$W/other/"
  sed -i 's|spike.installer = { path = "../installer", tools|spike.installer = { path = "../other", tools|' "$W/app/mcpp.toml"
  rm -rf "$W/app/target"
  run e42-mismatch "$W/app" build --features installer --strict
  reading E4.2-restated-different-path "warnings=$(grep -c '^warning' $ROOT/e42-mismatch.log) tool-from: $(grep -o 'Building host tool.*' $ROOT/e42-mismatch.log | head -1) $(grep -o 'SPIKE.*' $ROOT/e42-mismatch.log | sed "s|$HOME|~|g" | cut -c1-200); $(tail -1 $ROOT/e42-mismatch.log)"
  rm -rf "$W"/*/target
}


e41b() { # a forward to a dependency declared for another row, and to one behind an inactive feature
  local W=$ROOT/e41b; rm -rf "$W"; mkdir -p "$W"
  pkg "$W/win" spike win lib '[features]
x = []'
  echo 'int win_answer(){return 1;}' > "$W/win/src/win.cpp"
  pkg "$W/fw" spike fw lib '[target.'"'"'cfg(os = "windows")'"'"'.dependencies]
spike.win = { path = "../win" }

[features]
kotlin = ["spike.win/x"]'
  echo 'int fw_answer(){return 42;}' > "$W/fw/src/fw.cpp"
  pkg "$W/app" "" app bin '[dependencies]
spike.fw = { path = "../fw", features = ["kotlin"] }'
  printf 'int fw_answer();\nint main(){return fw_answer()==42?0:1;}\n' > "$W/app/src/main.cpp"
  run e41b-other-row "$W/app" build --strict
  reading E4.1b-forward-to-other-row-dep "$(grep -m1 -o 'error: .*' $ROOT/e41b-other-row.log | cut -c1-140); $(tail -1 $ROOT/e41b-other-row.log)"
  rm -rf "$W"/*/target
}


e6_fixture() { # W installer-edge-toml tool-dep-toml
  local W=$1; rm -rf "$W"; mkdir -p "$W"
  pkg "$W/fw" spike fw lib "[features]
installer = []

$2"
  echo 'int fw_answer() { return 42; }' > "$W/fw/src/fw.cpp"
  pkg "$W/fw/tool" spike fw-installer bin "$3"
  printf 'int fw_answer();\n#include <cstdio>\nint main() { std::printf("fw-installer ran %%d\\n", fw_answer()); return 0; }\n' > "$W/fw/tool/src/main.cpp"
  pkg "$W/app" "" app bin '[dependencies]
spike.fw = { path = "../fw" }

[features]
windows-installer = ["spike.fw/installer"]'
  printf 'int fw_answer();\nint main() { return fw_answer() == 42 ? 0 : 1; }\n' > "$W/app/src/main.cpp"
  cat > "$W/app/build.mcpp" <<'EOF2'
import std;
import mcpp;

int main() {
    const char* a = mcpp::dep_bin("fw-installer", "fw-installer");
    const char* b = mcpp::dep_bin("spike.fw-installer", "fw-installer");
    const std::string message = std::string("SPIKE short=[") + (a ? a : "") + "] qualified=[" + (b ? b : "") + "]";
    mcpp::warning(message.c_str());
    return 0;
}
EOF2
}
e6() { # the issue's fixture
  local W=$ROOT/e6
  e6_fixture "$W" "[target.'cfg(os = \"linux\")'.feature-deps.installer]
spike.fw-installer = { path = \"tool\", tools = [\"fw-installer\"], reexport = true }" '[dependencies]
spike.fw = { path = ".." }'
  run e6-build "$W/app" build --features windows-installer
  reading E6-issue-fixture "$(grep -o 'SPIKE.*' $ROOT/e6-build.log | sed "s|$HOME|~|g" | cut -c1-160) | $(grep -m1 -o 'error: .*' $ROOT/e6-build.log); $(tail -1 $ROOT/e6-build.log)"
  local tool=$(grep -o 'short=\[[^]]*' $ROOT/e6-build.log | sed 's/short=\[//')
  [ -n "$tool" ] && reading E6-tool-runs "$($tool)"
  run e6-nofeature "$W/app" build
  reading E6-without-feature "$(grep -m1 -o 'error: .*' $ROOT/e6-nofeature.log); $(tail -1 $ROOT/e6-nofeature.log)"
  # the resolved graph: which edges exist
  run e6-local "$W/app" build --features windows-installer --cache=local
  local exe=$(find "$W/app/target" -type f -perm -u+x -name app | head -1)
  reading E6-cache-local "$(grep -m1 -o 'error: .*' $ROOT/e6-local.log); $(tail -1 $ROOT/e6-local.log); app runs: $([ -n "$exe" ] && { "$exe"; echo exit=$?; })"
  reading E6-cache-local-link "$(grep -h '^build bin/app' $(find "$W/app/target" -name build.ninja) | sed 's|.*cxx_link||')"
  rm -rf "$W"/*/target "$W"/fw/tool/target
}
e6b() { # the tool graph REALLY activates the declaring feature
  local W=$ROOT/e6b
  e6_fixture "$W" "[target.'cfg(os = \"linux\")'.feature-deps.installer]
spike.fw-installer = { path = \"tool\", tools = [\"fw-installer\"], reexport = true }" '[dependencies]
spike.fw = { path = "..", features = ["installer"] }'
  run e6b-build "$W/app" build --features windows-installer
  reading E6b-tool-activates-declaring-feature "$(grep -c 'Building host tool' $ROOT/e6b-build.log) host-tool lines | $(grep -m1 -o 'error: .*' $ROOT/e6b-build.log | cut -c1-200); $(tail -1 $ROOT/e6b-build.log)"
  grep -A3 'error:' $ROOT/e6b-build.log | head -6 | sed "s|$HOME|~|g; s/^/  /"
  rm -rf "$W"/*/target "$W"/fw/tool/target
}
e6c() { # does a tool-only dependency's own library reach the consumer's link?
  local W=$ROOT/e6c
  e6_fixture "$W" "[feature-deps.installer]
spike.fw-installer = { path = \"tool\", tools = [\"fw-installer\"], reexport = true }" '[dependencies]
spike.z = { path = "../../z" }'
  pkg "$W/z" spike z lib ''
  echo 'int z_only_in_the_tool() { return 7; }' > "$W/z/src/z.cpp"
  printf 'int z_only_in_the_tool();\n#include <cstdio>\nint main() { std::printf("tool %%d\\n", z_only_in_the_tool()); return 0; }\n' > "$W/fw/tool/src/main.cpp"
  run e6c-build "$W/app" build --features windows-installer --verbose
  local exe=$(find "$W/app/target" -type f -name app -perm -u+x | head -1)
  reading E6c-tool-dependency-in-consumer "app link line:$(grep -h '^build bin/app' $(find "$W/app/target" -name build.ninja) | sed 's|.*cxx_link||'); app defines z symbol: $(nm -C "$exe" 2>/dev/null | grep -c z_only_in_the_tool); $(tail -1 $ROOT/e6c-build.log)"
  rm -rf "$W"/*/target "$W"/fw/tool/target
}
e6d() { # the same cycle without a feature: an unconditional [build-dependencies] tool edge
  local W=$ROOT/e6d
  e6_fixture "$W" '[build-dependencies]
spike.fw-installer = { path = "tool", tools = ["fw-installer"], reexport = true }' '[dependencies]
spike.fw = { path = ".." }'
  run e6d-build "$W/app" build
  reading E6d-build-dependency-tool-edge "$(grep -o 'SPIKE.*' $ROOT/e6d-build.log | sed "s|$HOME|~|g" | cut -c1-120) | $(grep -m1 -o 'error: .*' $ROOT/e6d-build.log); $(tail -1 $ROOT/e6d-build.log)"
  rm -rf "$W"/*/target "$W"/fw/tool/target
}


e7() { # a git dependency naming a workspace member of the repository
  local W=$ROOT/e7; rm -rf "$W"; mkdir -p "$W"
  pkg "$W/repo" spike fw lib '[workspace]
members = ["tool"]'
  echo 'int fw_answer() { return 42; }' > "$W/repo/src/fw.cpp"
  pkg "$W/repo/tool" spike fw-installer bin '[dependencies]
spike.fw = { path = ".." }'
  printf 'int fw_answer();\n#include <cstdio>\nint main() { std::printf("fw-installer ran %%d\\n", fw_answer()); return 0; }\n' > "$W/repo/tool/src/main.cpp"
  (cd "$W/repo" && git init -q && git add -A && git -c user.email=p@p -c user.name=p commit -qm init)
  local rev=$(git -C "$W/repo" rev-parse HEAD)
  local url="file://$W/repo"
  pkg "$W/app" "" app bin "[dependencies]
spike.fw = { git = \"$url\", rev = \"$rev\" }

[features]
windows-installer = []

[target.'cfg(os = \"linux\")'.feature-deps.windows-installer]
spike.fw-installer = { git = \"$url\", rev = \"$rev\", tools = [\"fw-installer\"] }"
  printf 'int fw_answer();\nint main() { return fw_answer() == 42 ? 0 : 1; }\n' > "$W/app/src/main.cpp"
  cat > "$W/app/build.mcpp" <<'EOF2'
import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("fw-installer", "fw-installer");
    mcpp::warning((std::string("SPIKE short=[") + (a ? a : "") + "]").c_str());
    return 0;
}
EOF2
  run e7-both "$W/app" build --features windows-installer
  reading E7-both-keys "$(grep -c 'that identity is used' $ROOT/e7-both.log) adoption warning(s) | $(grep -o 'SPIKE.*' $ROOT/e7-both.log) | $(grep -o 'Compiling spike.fw.*' $ROOT/e7-both.log) | $(grep -m1 -o 'error: .*' $ROOT/e7-both.log | cut -c1-160); $(tail -1 $ROOT/e7-both.log)"
  # the member alone, with no key naming the root package
  rm -rf "$W/app/target"
  sed -i '/^spike.fw = { git/d' "$W/app/mcpp.toml"
  sed -i 's/^int fw_answer();$//; s/return fw_answer() == 42 ? 0 : 1;/return 0;/' "$W/app/src/main.cpp"
  run e7-member-only "$W/app" build --features windows-installer
  reading E7-member-only "$(grep -m1 -o 'error: .*' $ROOT/e7-member-only.log | sed "s|$HOME|~|g" | cut -c1-200); $(tail -1 $ROOT/e7-member-only.log)"
  # control: the same member by path from a checkout works
  rm -rf "$W/app/target"
  sed -i "s|spike.fw-installer = { git = \"$url\", rev = \"$rev\",|spike.fw-installer = { path = \"../repo/tool\",|" "$W/app/mcpp.toml"
  run e7-member-by-path "$W/app" build --features windows-installer
  reading E7-member-by-path "$(grep -o 'SPIKE.*' $ROOT/e7-member-by-path.log | sed "s|$HOME|~|g" | cut -c1-120) | $(grep -m1 -o 'error: .*' $ROOT/e7-member-by-path.log); $(tail -1 $ROOT/e7-member-by-path.log)"
  rm -rf "$W"/app/target "$W"/repo/target "$W"/repo/tool/target
}

edup() { # one consumer naming one package in [dependencies] and [build-dependencies] with different requests
  local W=$ROOT/edup; rm -rf "$W"; mkdir -p "$W"
  mkdir -p "$W/installer/src"
  cat > "$W/installer/mcpp.toml" <<'EOF2'
[package]
name      = "installer"
namespace = "spike"
version   = "0.1.0"
standard  = "c++20"

[targets.installer-lib]
kind = "lib"

[targets.installer]
kind = "bin"
main = "src/main.cpp"

[build]
sources = ["src/lib.cpp"]
EOF2
  echo 'int installer_answer() { return 42; }' > "$W/installer/src/lib.cpp"
  echo 'int main() { return 0; }' > "$W/installer/src/main.cpp"
  pkg "$W/app" "" app bin '[dependencies]
spike.installer = { path = "../installer" }

[build-dependencies]
spike.installer = { path = "../installer", tools = ["installer"] }'
  printf 'int installer_answer();\nint main() { return installer_answer() == 42 ? 0 : 1; }\n' > "$W/app/src/main.cpp"
  cat > "$W/app/build.mcpp" <<'EOF2'
import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("installer", "installer");
    mcpp::warning((std::string("SPIKE short=[") + (a ? a : "") + "]").c_str());
    return 0;
}
EOF2
  run edup-two-tables "$W/app" build --strict
  reading EDUP-dependencies-and-build-dependencies "tool built: $(grep -c 'Building host tool' $ROOT/edup-two-tables.log) | $(grep -o 'SPIKE.*' $ROOT/edup-two-tables.log | sed "s|$HOME|~|g" | cut -c1-100) | $(grep -m1 -o 'error: .*' $ROOT/edup-two-tables.log); $(tail -1 $ROOT/edup-two-tables.log)"
  rm -rf "$W"/*/target
}

e8() { # --features with a dependency's feature, with and without a root [features] table
  local W=$ROOT/e8
  e6_fixture "$W" "[target.'cfg(os = \"linux\")'.feature-deps.installer]
spike.fw-installer = { path = \"tool\", tools = [\"fw-installer\"], reexport = true }" ''
  printf '#include <cstdio>\nint main() { std::puts("fw-installer ran"); return 0; }\n' > "$W/fw/tool/src/main.cpp"
  sed -i '/^\[features\]$/,$d' "$W/app/mcpp.toml"   # no [features] table in the application
  run e8-dep-feature-no-table "$W/app" build --strict --features spike.fw/installer
  reading E8-dep-feature-no-table "$(grep -o 'SPIKE.*' $ROOT/e8-dep-feature-no-table.log) | tool built: $(grep -c 'Building host tool' $ROOT/e8-dep-feature-no-table.log); $(tail -1 $ROOT/e8-dep-feature-no-table.log)"
  reading E8-dep-feature-no-table-macro "$(grep -o -- '-DMCPP_FEATURE_[A-Z_]*' $W/app/compile_commands.json | sort -u | tr '\n' ' ')"
  run e8-unknown-no-table "$W/app" build --strict --features nothing-here
  reading E8-unknown-no-table "$(grep -m1 -o 'error: .*' $ROOT/e8-unknown-no-table.log); $(tail -1 $ROOT/e8-unknown-no-table.log); macros: $(grep -o -- '-DMCPP_FEATURE_[A-Z_]*' $W/app/compile_commands.json | sort -u | tr '\n' ' ')"
  printf '\n[features]\nwindows-installer = []\n' >> "$W/app/mcpp.toml"
  run e8-dep-feature-table "$W/app" build --features spike.fw/installer
  reading E8-dep-feature-with-table "$(grep -m1 -o 'warning: --features.*' $ROOT/e8-dep-feature-table.log) | tool built: $(grep -c 'Building host tool' $ROOT/e8-dep-feature-table.log); $(tail -1 $ROOT/e8-dep-feature-table.log)"
  run e8-dep-feature-table-strict "$W/app" build --strict --features spike.fw/installer
  reading E8-dep-feature-with-table-strict "$(grep -m1 -o 'error: .*' $ROOT/e8-dep-feature-table-strict.log); $(tail -1 $ROOT/e8-dep-feature-table-strict.log)"
  # control: the manifest forward the CLI form would replace
  sed -i 's/^windows-installer = \[\]$/windows-installer = ["spike.fw\/installer"]/' "$W/app/mcpp.toml"
  run e8-manifest-forward "$W/app" build --strict --features windows-installer
  reading E8-manifest-forward-control "$(grep -o 'SPIKE.*' $ROOT/e8-manifest-forward.log | sed "s|$HOME|~|g" | cut -c1-110) | tool built: $(grep -c 'Building host tool' $ROOT/e8-manifest-forward.log); $(tail -1 $ROOT/e8-manifest-forward.log)"
  rm -rf "$W"/*/target "$W"/fw/tool/target
}
case "${1:-all}" in
  all) e41; e41b; e42; e6; e6b; e6c; e6d; e7; edup; e8 ;;
  *) for f in "$@"; do "$f"; done ;;
esac
__PROBES_646_649_GROUP_C__
  ROOT="$WORK/c" bash "$WORK/c.sh" all </dev/null
}

group_d() { mkdir -p "$WORK/d"; cat > "$WORK/d.sh" <<'__PROBES_646_649_GROUP_D__'
# Group d: probes for #648. Each prints READING lines. Linux x86_64, mcpp 2026.9.15.2.
set +e
M=${M:-~/.xlings/data/xpkgs/xim-x-mcpp/2026.9.15.2/bin/mcpp}
W=${W:-$(mktemp -d)}

# ── A2: the descriptors a child started during planning inherits ──────────
# A build program lists /proc/self/fd; the reader prints the inode of the pipe
# it reads mcpp's stdout from. A child fd naming the same inode is the leak.
mkdir -p "$W/a2/src" && cd "$W/a2" || exit 1
cat > mcpp.toml <<'T'
[package]
name     = "a2probe"
version  = "0.1.0"
standard = "c++23"

[targets.a2probe]
kind = "bin"
main = "src/main.cpp"
T
echo 'int main() { return 0; }' > src/main.cpp
cat > build.mcpp <<T
import std;
import mcpp;
int main() {
    std::ofstream out("$W/a2/fds.txt");
    for (auto& e : std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code ec;
        out << e.path().filename().string() << " -> "
            << std::filesystem::read_symlink(e.path(), ec).string() << "\n";
    }
    return 0;
}
T
MCPP_OFFLINE=1 "$M" emit build-database --format json 2>/dev/null \
  | { echo "READING A2 reader stdin=$(readlink /proc/self/fd/0)"; cat >/dev/null; }
sed 's/^/READING A2 child fd /' "$W/a2/fds.txt"

# ── T: what triggered the refresh on openxlings/xlings ────────────────────
# Offline, the refresh decision is still computed; a dependency reported as
# "offline mode" is one the decision wanted to refresh. The same run resolves it.
XL=${XL:-$HOME/workspace/github/openxlings/xlings}
if [ -f "$XL/mcpp.toml" ]; then
  (cd "$XL" && MCPP_OFFLINE=1 timeout 180 "$M" emit build-database --format json -v \
      2>"$W/xl.err" >"$W/xl.json"; echo "READING T exit=$?")
  grep -a "index: .*offline mode" "$W/xl.err" | sed 's/\x1b\[[0-9;]*m//g; s/^.*index: /READING T decision /'
  grep -a "resolved to .* through the deprecated bare-name search" "$W/xl.err" | sed 's/^/READING T resolver /'
fi

# ── A6: the GitCode mirror of the mcpp-index artifact against its pointer ──
ptr=$(curl -sSL https://raw.githubusercontent.com/xlings-res/mcpp-index/main/mcpp-index-pointers.json)
ver=$(printf '%s' "$ptr" | python3 -c 'import json,sys; print(json.load(sys.stdin)["indexes"]["mcpp"]["index_version"])')
sha=$(printf '%s' "$ptr" | python3 -c 'import json,sys; print(json.load(sys.stdin)["indexes"]["mcpp"]["artifact"]["sha256"])')
echo "READING A6 pointer version=$ver sha=$sha"
for host in github gitcode; do
  url="https://$host.com/xlings-res/mcpp-index/releases/download/v$ver/mcpp-index-$ver.tar.gz"
  curl -sSL -o "$W/$host.tgz" "$url"
  echo "READING A6 $host sha=$(sha256sum "$W/$host.tgz" | cut -d' ' -f1) bytes=$(wc -c < "$W/$host.tgz")"
  echo "READING A6 $host first-entry=$(tar -tvzf "$W/$host.tgz" | head -1)"
done
__PROBES_646_649_GROUP_D__
  W="$WORK/d" bash "$WORK/d.sh" </dev/null
}

case "$WHICH" in
  a) group_a ;;
  b) group_b ;;
  c) group_c ;;
  d) group_d ;;
  all) group_a; group_b; group_c; group_d ;;
  *) echo "usage: $0 [a|b|c|d|all] [work dir]" >&2; exit 2 ;;
esac
