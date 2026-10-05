#!/usr/bin/env bash
# TEMPORARY PROBE (do not merge): facts about cl.exe /GL, /bigobj, link /LTCG
# and lib /LTCG that the design for #770 relies on. Every command prints its
# output and its exit code; nothing here asserts.
set -u
export MSYS2_ARG_CONV_EXCL='*'   # keep /GL etc. away from MSYS path conversion
W="$RUNNER_TEMP/msvc-facts"; rm -rf "$W"; mkdir -p "$W"; cd "$W"

run() { echo "\$ $*"; "$@" 2>&1; echo "rc=$?"; }
section() { echo; echo "### $1"; }
hex() { od -A d -t x1 -N 64 "$1"; }

section "toolset"
cl 2>&1 | head -1
echo "VCToolsInstallDir=$VCToolsInstallDir"

cat > a.cpp <<'EOF'
extern "C" int probe_fn(int x) { return x + 1; }
int probe_var = 7;
EOF
cat > main.cpp <<'EOF'
extern "C" int probe_fn(int);
int main() { return probe_fn(-1); }
EOF

section "F1 object headers (first 64 bytes)"
run cl -nologo -c a.cpp -Fo:plain.obj
run cl -nologo -c -bigobj a.cpp -Fo:big.obj
run cl -nologo -c -GL a.cpp -Fo:gl.obj
run cl -nologo -c -GL -bigobj a.cpp -Fo:glbig.obj
for f in plain big gl glbig; do echo "-- $f.obj"; hex "$f.obj"; done
# A short import object: a member of an import library.
printf 'LIBRARY probe\nEXPORTS\n  probe_fn\n' > probe.def
run lib -nologo -def:probe.def -machine:x64 -out:probe_imp.lib
run lib -nologo -list probe_imp.lib

section "F5 dumpbin on a /GL object"
run dumpbin -nologo -symbols gl.obj

section "F2 link /GL objects WITHOUT /LTCG"
run cl -nologo -c -GL main.cpp -Fo:main_gl.obj
run link -nologo main_gl.obj gl.obj -out:noltcg.exe
run ./noltcg.exe

section "F3 lib.exe on /GL objects, without and with /LTCG"
run lib -nologo gl.obj -out:gl_noltcg.lib
run lib -nologo -LTCG gl.obj -out:gl_ltcg.lib

section "F4 /GL with /Zi /FS, and /LTCG with /DEBUG"
run cl -nologo -c -GL -Zi -FS a.cpp -Fo:gl_zi.obj
run link -nologo -LTCG -DEBUG main_gl.obj gl_zi.obj -out:ltcg_debug.exe
run link -nologo -LTCG -DEBUG -INCREMENTAL main_gl.obj gl_zi.obj -out:ltcg_debug_inc.exe
run link -nologo -LTCG -DEBUG -INCREMENTAL:NO main_gl.obj gl_zi.obj -out:ltcg_debug_noinc.exe

section "F8 a /GL DLL exports through __declspec(dllexport)"
cat > dll.cpp <<'EOF'
extern "C" __declspec(dllexport) int dll_fn(int x) { return x * 2; }
EOF
run cl -nologo -c -GL dll.cpp -Fo:dll_gl.obj
run link -nologo -DLL -LTCG dll_gl.obj -out:gl.dll
run dumpbin -nologo -exports gl.dll

section "D5 per-target degrade: own objects plain + a /GL static library + .def"
cat > own.cpp <<'EOF'
extern "C" int helper(int);
extern "C" int own_fn(int x) { return helper(x) + 1; }
EOF
cat > helper.cpp <<'EOF'
extern "C" int helper(int x) { return x * 3; }
EOF
printf 'LIBRARY own\nEXPORTS\n  own_fn\n' > own.def
run cl -nologo -c own.cpp -Fo:own.obj
run cl -nologo -c -GL helper.cpp -Fo:helper_gl.obj
run lib -nologo -LTCG helper_gl.obj -out:helper_gl.lib
echo "-- own.obj is ordinary COFF:"; hex own.obj | head -1
run link -nologo -DLL -LTCG own.obj helper_gl.lib -def:own.def -out:own.dll
run dumpbin -nologo -exports own.dll
run link -nologo -DLL own.obj helper_gl.lib -def:own.def -out:own_noltcg.dll

section "O2 import std across /GL boundaries"
STD_IXX="${VCToolsInstallDir}modules\\std.ixx"
cat > m.cpp <<'EOF'
import std;
int main() { std::vector<int> v{1, 2, 3}; return v.size() == 3 ? 0 : 1; }
EOF
mkdir -p plainstd glstd
echo "-- std built WITHOUT /GL, consumer WITH /GL"
run cl -nologo -std:c++latest -EHsc -c "$STD_IXX" -ifcOutput plainstd/std.ifc -Fo:plainstd/std.obj
run cl -nologo -std:c++latest -EHsc -GL -c m.cpp -reference std=plainstd/std.ifc -Fo:m_gl.obj
run link -nologo -LTCG m_gl.obj plainstd/std.obj -out:m1.exe
run ./m1.exe
echo "-- std built WITH /GL, consumer WITHOUT /GL"
run cl -nologo -std:c++latest -EHsc -GL -c "$STD_IXX" -ifcOutput glstd/std.ifc -Fo:glstd/std.obj
run cl -nologo -std:c++latest -EHsc -c m.cpp -reference std=glstd/std.ifc -Fo:m_plain.obj
run link -nologo -LTCG m_plain.obj glstd/std.obj -out:m2.exe
run ./m2.exe
echo "-- both WITH /GL"
run link -nologo -LTCG m_gl.obj glstd/std.obj -out:m3.exe
run ./m3.exe
