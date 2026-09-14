#!/usr/bin/env bash
# Probes for mcpp-community/mcpp#641 and #642, as recorded in
# 2026-09-15-641-642-link-forms-standards-and-paths.md (§6).
#
# Host: Linux x86_64. Engine: the released mcpp 2026.9.15.1 (the xlings
# package, whose home is ~/.mcpp). Toolchains already in that home:
# llvm@22.1.8 and gcc@16.1.0; package llvm.libcxx 22.1.8.1 installed.
# M1 uses the network (a fresh home fetches its index).
#
# Every reading is a line starting with READING; nothing else is a result.
#
# Usage: 2026-09-15-641-642-probes.sh <empty work directory>
set -u
WORK=${1:?usage: $0 <empty work directory>}
mkdir -p "$WORK" && WORK=$(cd "$WORK" && pwd)
M=${MCPP:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/2026.9.15.1/bin/mcpp}
XLINGS=${MCPP_VENDORED_XLINGS:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/2026.9.15.1/registry/bin/xlings}
LIBCXX=$HOME/.mcpp/registry/data/xpkgs/llvm-x-libcxx/22.1.8.1/libcxx-22.1.8.1
NINJA=$(find "$HOME/.mcpp/registry/data/xpkgs/xim-x-ninja" -name ninja -type f | head -1)
# An outer SubOS must not steer the engine's registry (2026.9.14.3).
unset XLINGS_ACTIVE_SUBOS
reading() { echo "READING $*"; }
"$M" --version | sed 's/^/READING engine: /'

# --- M1 (#641 item 1): a required llvm on a home with no llvm installed ------
mkdir -p "$WORK/m1/home" "$WORK/m1/proj/src" && cd "$WORK/m1/proj"
printf '[package]\nname = "m1"\nversion = "0.1.0"\nrequires = ["mcpp:compiler=llvm"]\n' > mcpp.toml
printf 'int main() { return 0; }\n' > src/main.cpp
MCPP_HOME="$WORK/m1/home" MCPP_VENDORED_XLINGS="$XLINGS" XLINGS_NON_INTERACTIVE=1 \
    timeout 900 "$M" build > build.log 2>&1
reading "M1 exit=$? spec=$(grep -o "toolchain 'llvm@[^']*'" build.log | head -1)"
# The same comparison, from the other side: a family whose payload is not
# named after the family finds no pin at all, although rows pin both.
for fam in emsdk android-ndk; do
    mkdir -p "$WORK/m1/$fam/src" && cd "$WORK/m1/$fam"
    printf '[package]\nname = "p"\nversion = "0.1.0"\nrequires = ["mcpp:compiler=%s"]\n' "$fam" > mcpp.toml
    printf 'int main() { return 0; }\n' > src/main.cpp
    MCPP_HOME="$WORK/m1/home" XLINGS_NON_INTERACTIVE=1 timeout 300 "$M" build > build.log 2>&1
    reading "M1 $fam exit=$? $(grep -o 'none is installed, and no target row pins one' build.log)"
done

# --- M2 (#641 item 2): a c++20 root over llvm.libcxx -------------------------
mkdir -p "$WORK/m2/src" && cd "$WORK/m2"
cat > mcpp.toml <<'T'
[package]
name     = "m2"
version  = "0.1.0"
standard = "c++20"

[toolchain]
default = "llvm@22.1.8"

[dependencies]
llvm.libcxx = "22.1.8.1"
T
cat > src/main.cpp <<'C'
import std;
int main() {
    std::unordered_map<std::string, int> m; m["one"] = 1;
    try { throw std::runtime_error("x"); } catch (const std::runtime_error&) { m["two"] = 2; }
    std::cout << std::format("{}-{}", m["one"], m["two"]) << std::endl;
}
C
timeout 1800 "$M" build > build.log 2>&1
reading "M2 exit=$? resize_and_overwrite errors=$(grep -c "no member named 'resize_and_overwrite'" build.log) bad_expected_access errors=$(grep -c "no template named 'bad_expected_access'" build.log)"

# --- M2b: the same root, with the package's implementation units at c++23 ----
# `[build] cxxflags` refuses `-std=`, so the emulation uses the conditional
# table the package already has for Linux. The std module is still built at
# the graph's level, which is the arrangement §3.2 proposes.
mkdir -p "$WORK/m2b/src" && cd "$WORK/m2b"
cp -r "$LIBCXX" libcxx && chmod -R u+w libcxx
python3 - <<'PY'
p = 'libcxx/mcpp.toml'; s = open(p).read()
old = 'cxxflags = ["-D_GNU_SOURCE"]'
assert s.count(old) == 1, s.count(old)
s = s.replace(old, 'cxxflags = ["-D_GNU_SOURCE", "-std=c++23"]')
assert s.count('-std=c++23') == 1
open(p, 'w').write(s)
PY
sed 's/^llvm.libcxx = .*$//; s/^name     = "m2"$/name     = "m2b"/' "$WORK/m2/mcpp.toml" > mcpp.toml
printf '[dependencies.llvm.libcxx]\npath = "libcxx"\n' >> mcpp.toml
cp "$WORK/m2/src/main.cpp" src/main.cpp
timeout 1800 "$M" build > build.log 2>&1
reading "M2b exit=$?"
d=$(ls -d target/*/*/ 2>/dev/null | head -1)
if [ -n "$d" ]; then
    reading "M2b graph -std: $(grep -m1 '^cxxflags' "$d/build.ninja" | grep -o -- '-std=[^ ]*')"
    ln=$(grep -n 'libcxx/src/new.o *:' "$d/build.ninja" | head -1 | cut -d: -f1)
    reading "M2b new.cpp unit -std: $(sed -n "${ln},$((ln+6))p" "$d/build.ninja" | grep -o -- '-std=[^ ]*' | tr '\n' ' ')"
    [ -x "$d/bin/m2b" ] && reading "M2b run: out=$("$d/bin/m2b") exit=$?"
fi

# --- M3 (#641 item 5): a shared C++ dependency in a graph over llvm.libcxx ---
mkdir -p "$WORK/m3/app/src" "$WORK/m3/fw/src" && cd "$WORK/m3"
printf '[package]\nname = "fw"\nversion = "0.1.0"\n\n[targets.fw]\nkind = "shared"\n' > fw/mcpp.toml
cat > fw/src/fw.cppm <<'C'
export module fw;
import std;
export std::string fw_greet(int n);
export void fw_throw_runtime();
export void fw_throw_custom();
export struct fw_error : std::runtime_error { using std::runtime_error::runtime_error; };
C
cat > fw/src/fw.cpp <<'C'
module fw;
import std;
std::string fw_greet(int n) { return std::format("fw-{}", n); }
void fw_throw_runtime() { throw std::runtime_error("from fw"); }
void fw_throw_custom() { throw fw_error("custom from fw"); }
C
cat > app/mcpp.toml <<'T'
[package]
name    = "app"
version = "0.1.0"

[toolchain]
default = "llvm@22.1.8"

[dependencies]
llvm.libcxx = "22.1.8.1"
fw = { path = "../fw" }
T
cat > app/src/main.cpp <<'C'
import std;
import fw;
int main() {
    std::string s = fw_greet(3);
    std::cout << s << std::endl;
    try { fw_throw_runtime(); }
    catch (const std::runtime_error& e) { std::cout << "runtime_error caught by type" << std::endl; }
    catch (...) { std::cout << "runtime_error NOT matched by type" << std::endl; }
    try { fw_throw_custom(); }
    catch (const fw_error&) { std::cout << "fw_error caught by type" << std::endl; }
    catch (...) { std::cout << "fw_error NOT matched" << std::endl; }
}
C
cd app
timeout 1800 "$M" build > build.log 2>&1
reading "M3 exit=$? $(grep -m1 -o "non-exported symbol '[^']*' in '[^']*' is referenced by DSO 'bin/libfw.so'" build.log)"
d=$(ls -d target/*/*/ 2>/dev/null | head -1)
if [ -n "$d" ] && [ -f "$d/bin/libfw.so" ]; then
    cd "$d"
    reading "M3 libfw.so inputs: $(grep '^build bin/libfw.so' build.ninja | sed 's/^build bin\/libfw.so : cxx_shared //')"
    reading "M3 libfw.so unit_ldflags: $(grep -A3 '^build bin/libfw.so' build.ninja | grep -o 'unit_ldflags = .*')"
    reading "M3 libfw.so undefined std::__1 references: $(nm -D --undefined-only bin/libfw.so | grep -c '__1')"

    # --- M5: link each image with its own copy of the graph libc++ ---------
    fwcmd=$("$NINJA" -t commands bin/libfw.so | tail -1)
    appcmd=$("$NINJA" -t commands bin/app | tail -1)
    fwin=$(grep '^build bin/libfw.so' build.ninja | sed 's/^build bin\/libfw.so : cxx_shared //; s/ || .*//; s/ | .*//')
    appin=$(grep '^build bin/app ' build.ninja | sed 's/^build bin\/app : cxx_link //; s/ || .*//; s/ | .*//')
    rt=$(find obj/llvm_libcxx -name '*.o' | sort | tr '\n' ' ')
    printf '%s %s\n' "$fwin" "$rt" > bin/libfw.so.rsp
    printf '%s\n' "$appin" > bin/app.rsp
    eval "$fwcmd" > m5-fw.log 2>&1;  reading "M5 libfw.so linked with a private libc++: exit=$?"
    eval "$appcmd" > m5-app.log 2>&1; reading "M5 program linked: exit=$?"
    out=$(./bin/app 2>&1); rc=$?
    reading "M5 run exit=$rc output: $(echo "$out" | tr '\n' '|')"
fi

# --- M3b (finding F1): a shared package over a static package, no runtime ----
mkdir -p "$WORK/m3b/app/src" "$WORK/m3b/fw/src" "$WORK/m3b/x/src" && cd "$WORK/m3b"
printf '[package]\nname = "x"\nversion = "0.1.0"\n[build]\nsources = ["src/*.c"]\n[targets.x]\nkind = "lib"\n' > x/mcpp.toml
printf 'int x_answer(void) { return 41; }\n' > x/src/x.c
printf '[package]\nname = "fw"\nversion = "0.1.0"\n[build]\nsources = ["src/*.c"]\n[targets.fw]\nkind = "shared"\n[dependencies.x]\npath = "../x"\n' > fw/mcpp.toml
printf 'extern int x_answer(void);\nint fw_answer(void) { return x_answer() + 1; }\n' > fw/src/fw.c
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.fw]\npath = "../fw"\n' > app/mcpp.toml
printf 'extern "C" int fw_answer(void);\nint main() { return fw_answer() == 42 ? 0 : 1; }\n' > app/src/main.cpp
cd app
timeout 900 "$M" build > build.log 2>&1
reading "M3b exit=$?"
so=$(ls target/*/*/bin/libfw.so 2>/dev/null | head -1)
if [ -n "$so" ]; then
    bin=$(ls target/*/*/bin/app | head -1)
    reading "M3b libfw.so undefined x_answer=$(nm -D --undefined-only "$so" | grep -c x_answer) program exports x_answer=$(nm -D --defined-only "$bin" | grep -c x_answer)"
    "$bin"; reading "M3b run exit=$?"
fi

# --- M4 (#641 item 4): pack takes no --features ------------------------------
"$M" pack --features x > pack.log 2>&1
reading "M4 pack exit=$? $(head -1 pack.log)"

# --- M6 (#641 item 2): the key the two C++-layer packages write --------------
mkdir -p "$WORK/m6/src" && cd "$WORK/m6"
printf '[package]\nname = "m6"\nversion = "0.1.0"\n\n[build]\ncxx_standard = "c++23"\n' > mcpp.toml
printf 'int main() { return 0; }\n' > src/main.cpp
"$M" build > build.log 2>&1
reading "M6 exit=$? $(grep -o "\[build\] has unsupported key 'cxx_standard' (ignored)" build.log)"
