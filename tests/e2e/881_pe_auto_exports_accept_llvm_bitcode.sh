#!/usr/bin/env bash
# requires: msvc python3
# 881 -- mcpp#762: PE auto-export accepts LLVM LTO objects while preserving an
# explicitly annotated surface. Opting out removes the scanner from the graph.
set -e
source "$(dirname "$0")/_host_path.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"
REGISTRY_HOST=$(host_path "${MCPP_HOME:-$HOME/.mcpp}/registry")
export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"
cat > "$MCPP_HOME/config.toml" <<EOF
[xlings]
home = "$REGISTRY_HOST"
EOF
cd "$TMP"

# A: an unannotated module DLL, a separate implementation and exported data.
# A consumer proves the module initializer and code
# exports work with LTO; ctypes independently reads the exported data.
mkdir -p automatic/src app/src
cat > automatic/mcpp.toml <<'EOF'
[package]
name = "automatic"
version = "0.1.0"
[toolchain]
windows = "llvm@20.1.7"
[profile.release]
lto = true
[targets.automatic]
kind = "shared"
EOF
printf 'export module t881_lto;\nexport int answer();\n' > automatic/src/api.cppm
printf 'module t881_lto;\nint answer() { return 42; }\n' > automatic/src/impl.cpp
printf 'extern "C" int exported_data = 9;\n' > automatic/src/data.cpp
cat > app/mcpp.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"
[dependencies]
automatic = { path = "../automatic" }
[toolchain]
windows = "llvm@20.1.7"
EOF
printf 'import t881_lto;\nint main() { return answer() == 42 ? 0 : 1; }\n' > app/src/main.cpp
(cd automatic && "$MCPP" build --profile release > build.log 2>&1) || fail "A: the LTO DLL did not build" automatic/build.log
(cd app && "$MCPP" run --profile release > run.log 2>&1) || fail "A: the module consumer did not run" app/run.log
automatic=$(find automatic/target -name automatic.dll | head -1)
python3 - "$automatic" <<'PY'
import ctypes, os, sys
lib = ctypes.CDLL(os.path.abspath(sys.argv[1]))
assert ctypes.c_int.in_dll(lib, "exported_data").value == 9
PY

# Find the selected LLVM through its actual compile command, not another LLVM
# on PATH or a version that happens to be installed beside it.
db=$(find automatic/target -name compile_commands.json | head -1)
compiler=$(python3 - "$db" <<'PY'
import json, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
print(next(e["arguments"][0] for e in entries if e["file"].endswith("data.cpp")))
PY
)
nm="$(dirname "$compiler")/llvm-nm.exe"

# B: annotations in bitcode define the whole surface, including DATA. A
# literal containing export-like text must not be mistaken for an annotation.
mkdir -p annotated/src
cat > annotated/mcpp.toml <<'EOF'
[package]
name = "annotated"
version = "0.1.0"
[toolchain]
windows = "llvm@20.1.7"
[profile.release]
lto = true
[targets.annotated]
kind = "shared"
EOF
cat > annotated/src/api.cpp <<'EOF'
extern "C" __declspec(dllexport) int chosen() { return 7; }
extern "C" __declspec(dllexport) int chosen_data = 11;
extern "C" int hidden() { return 3; }
EOF
(cd annotated && "$MCPP" build --profile release > build.log 2>&1) || fail "B: annotated bitcode did not build" annotated/build.log
annotated=$(find annotated/target -name annotated.dll | head -1)
python3 - "$annotated" <<'PY'
import ctypes, os, sys
lib = ctypes.CDLL(os.path.abspath(sys.argv[1]))
assert lib.chosen() == 7
assert ctypes.c_int.in_dll(lib, "chosen_data").value == 11
try:
    lib.hidden
except AttributeError:
    pass
else:
    raise AssertionError("the scanner widened the annotated surface")
PY
def=$(find annotated/target -name annotated.def | head -1)
[ "$(grep -cvE '^(LIBRARY|EXPORTS|[[:space:]]*$)' "$def" || true)" -eq 0 ] || fail "B: an annotated library received automatic exports" "$def"

# C: native-only export control never depends on coff-def.
printf '\n' >> annotated/mcpp.toml
sed 's/kind = "shared"/kind = "shared"\nwindows_auto_export = false/' annotated/mcpp.toml > native.toml
mv native.toml annotated/mcpp.toml
(cd annotated && "$MCPP" build --profile release > native.log 2>&1) || fail "C: opt-out did not build" annotated/native.log
nj=$(find annotated/target -name build.ninja | head -1)
if grep -qE '^build .* : coff_def' "$nj"; then fail "C: the opt-out still schedules export discovery" "$nj"; fi

# The same target is also read through a path dependency, not just as the root.
mkdir -p native-consumer/src
cat > native-consumer/mcpp.toml <<'EOF'
[package]
name = "native-consumer"
version = "0.1.0"
[dependencies]
annotated = { path = "../annotated" }
[toolchain]
windows = "llvm@20.1.7"
EOF
printf 'extern "C" __declspec(dllimport) int chosen();\nint main() { return chosen() == 7 ? 0 : 1; }\n' > native-consumer/src/main.cpp
(cd native-consumer && "$MCPP" run --profile release > run.log 2>&1) || fail "C: dependency opt-out did not run" native-consumer/run.log
nj=$(find native-consumer/target -name build.ninja | head -1)
if grep -qE '^build .* : coff_def' "$nj"; then fail "C: the dependency opt-out still schedules export discovery" "$nj"; fi

# D: an ordinary annotated COFF input takes precedence in either input order.
# Deliberately unavailable LLVM tools prove that the bitcode inspection and
# symbol enumeration paths are not entered once COFF already states the intent.
"$compiler" --driver-mode=g++ --target=x86_64-pc-windows-msvc -c annotated/src/api.cpp -o annotated.obj
"$compiler" --driver-mode=g++ --target=x86_64-pc-windows-msvc -flto=thin -c automatic/src/data.cpp -o thin.obj
for order in 'annotated.obj thin.obj' 'thin.obj annotated.obj'; do
    "$MCPP" coff-def --output mixed.def --name mixed --llvm-cxx absent-clang --llvm-nm absent-nm $order > mixed.log 2>&1 || fail "D: annotation precedence depends on input order" mixed.log
    [ "$(grep -cvE '^(LIBRARY|EXPORTS|[[:space:]]*$)' mixed.def || true)" -eq 0 ] || fail "D: mixed objects widened the surface" mixed.def
done

# E: ThinLTO and textual export-like data. Only real IR declarations or linker
# option metadata express intent; a string literal does not disable auto-export.
cat > literal.cpp <<'EOF'
extern "C" int literal_value = 13;
extern "C" const char* message = "dllexport /EXPORT:pretend";
EOF
"$compiler" --driver-mode=g++ --target=x86_64-pc-windows-msvc -flto=thin -c literal.cpp -o literal.obj
"$MCPP" coff-def --output literal.def --name literal --llvm-cxx "$compiler" --llvm-nm "$nm" --llvm-target x86_64-pc-windows-msvc literal.obj > literal.log 2>&1 || fail "E: ThinLTO candidates were not read" literal.log
grep -q 'literal_value DATA' literal.def || fail "E: literal text suppressed automatic exports" literal.def

# F: pragma linker options in ThinLTO suppress discovery without llvm-nm.
cat > pragma.cpp <<'EOF'
#pragma comment(linker, "/EXPORT:pragma_api")
extern "C" int pragma_api() { return 19; }
extern "C" int unrelated() { return 1; }
EOF
"$compiler" --driver-mode=g++ --target=x86_64-pc-windows-msvc -flto=thin -c pragma.cpp -o pragma.obj
"$MCPP" coff-def --output pragma.def --name pragma --llvm-cxx "$compiler" --llvm-nm absent-nm --llvm-target x86_64-pc-windows-msvc pragma.obj > pragma.log 2>&1 || fail "F: linker metadata did not suppress candidate discovery" pragma.log
[ "$(grep -cvE '^(LIBRARY|EXPORTS|[[:space:]]*$)' pragma.def || true)" -eq 0 ] || fail "F: linker metadata widened the surface" pragma.def

# G: ordinary COFF and bitcode contribute to the same unannotated surface.
printf 'extern "C" int native_api() { return 23; }\n' > native.cpp
"$compiler" --driver-mode=g++ --target=x86_64-pc-windows-msvc -c native.cpp -o native.obj
"$MCPP" coff-def --output union.def --name union --llvm-cxx "$compiler" --llvm-nm "$nm" --llvm-target x86_64-pc-windows-msvc native.obj literal.obj > union.log 2>&1 || fail "G: mixed candidate discovery failed" union.log
grep -q 'native_api$' union.def || fail "G: COFF function was lost" union.def
grep -q 'literal_value DATA' union.def || fail "G: bitcode data was lost" union.def

# H: the selected target controls x86 decoration, not the host's architecture.
"$compiler" --driver-mode=g++ --target=i686-pc-windows-msvc -flto=thin -c literal.cpp -o x86.obj
"$MCPP" coff-def --output x86.def --name x86 --llvm-cxx "$compiler" --llvm-nm "$nm" --llvm-target i686-pc-windows-msvc x86.obj > x86.log 2>&1 || fail "H: x86 bitcode discovery failed" x86.log
grep -q 'literal_value DATA' x86.def || fail "H: x86 cdecl decoration was not normalized" x86.def

# I: `exports` narrows what discovery finds (#766). Only the matching symbols
# are published, data keeps its DATA keyword, and the rest is not exported.
mkdir -p narrowed/src
cat > narrowed/mcpp.toml <<'EOF'
[package]
name = "narrowed"
version = "0.1.0"
[toolchain]
windows = "llvm@20.1.7"
[targets.narrowed]
kind = "shared"
exports = ["keep_*"]
EOF
cat > narrowed/src/api.cpp <<'EOF'
extern "C" int keep_api() { return 29; }
extern "C" int keep_value = 31;
extern "C" int drop_api() { return 37; }
EOF
(cd narrowed && "$MCPP" build > build.log 2>&1) || fail "I: the narrowed DLL did not build" narrowed/build.log
narrowedDef=$(find narrowed/target -name narrowed.def | head -1)
grep -q 'keep_api$' "$narrowedDef" || fail "I: a matching function was not exported" "$narrowedDef"
grep -q 'keep_value DATA' "$narrowedDef" || fail "I: matching data lost its DATA keyword" "$narrowedDef"
if grep -q 'drop_api' "$narrowedDef"; then fail "I: a symbol outside the patterns was exported" "$narrowedDef"; fi
narrowedDll=$(find narrowed/target -name narrowed.dll | head -1)
python3 - "$narrowedDll" <<'PY'
import ctypes, os, sys
lib = ctypes.CDLL(os.path.abspath(sys.argv[1]))
assert lib.keep_api() == 29
assert ctypes.c_int.in_dll(lib, "keep_value").value == 31
try:
    lib.drop_api
except AttributeError:
    pass
else:
    raise AssertionError("drop_api is published")
PY

# J: beside source annotations `exports` cannot narrow, and says so.
printf 'keep_*\n' > patterns.txt
"$MCPP" coff-def --output warned.def --name warned --exports-file patterns.txt annotated.obj > warned.log 2>&1 \
    || fail "J: coff-def failed beside annotations" warned.log
grep -q '`exports` has no effect on this DLL' warned.log || fail "J: no warning beside annotations" warned.log

# K: `exports` with discovery off has nothing to narrow, and is refused when
# an MSVC-ABI row is planned.
mkdir -p contradictory/src
cat > contradictory/mcpp.toml <<'EOF'
[package]
name = "contradictory"
version = "0.1.0"
[toolchain]
windows = "llvm@20.1.7"
[targets.contradictory]
kind = "shared"
exports = ["api_*"]
windows_auto_export = false
EOF
printf 'extern "C" int api_one() { return 1; }\n' > contradictory/src/api.cpp
if (cd contradictory && "$MCPP" build > build.log 2>&1); then
    fail "K: exports with windows_auto_export = false was accepted" contradictory/build.log
fi
grep -q 'windows_auto_export = false' contradictory/build.log || fail "K: the refusal did not name the keys" contradictory/build.log

# L: a DLL a consumer links must export something; one nothing links may not.
printf 'static int internal_only() { return 1; }\n' > empty.cpp
"$compiler" --driver-mode=g++ --target=x86_64-pc-windows-msvc -c empty.cpp -o empty.obj
"$MCPP" coff-def --output empty.def --name empty empty.obj > empty.log 2>&1 \
    || fail "L: an empty surface without a consumer was refused" empty.log
if "$MCPP" coff-def --output empty.def --name empty --required empty.obj > required.log 2>&1; then
    fail "L: an empty surface a consumer links was accepted" required.log
fi
grep -q 'exports no symbol, and a consumer in this build links it' required.log || fail "L: the refusal was not stated" required.log
echo "PASS: 881_pe_auto_exports_accept_llvm_bitcode"
