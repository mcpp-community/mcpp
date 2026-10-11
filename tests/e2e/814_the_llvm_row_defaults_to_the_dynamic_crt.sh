#!/usr/bin/env bash
# requires: windows
# 814_the_llvm_row_defaults_to_the_dynamic_crt.sh -- #718: the CRT model is a
# property of the MSVC ABI, not of the compiler. clang++ targeting
# `x86_64-windows-msvc` (the LLVM row) used to receive no CRT model at all and
# always linked the static CRT (#649 E10, inverted at 703); it now receives
# the same model cl.exe does, spelled `-fms-runtime-lib=static`/`=dll`, and
# `toolchain-coupled` (the dynamic CRT, with the toolset's own
# vcruntime140.dll/msvcp140.dll staged beside the artifact) is the default.
#
# This checks the LLVM row's half of that: the default program imports
# vcruntime140.dll and runs from a clean PATH because the file is staged
# beside it; `cxx_runtime = "self-contained"` restores the static CRT and the
# import disappears; switching between the two keeps both std BMIs valid
# (neither clobbers the other); and `mcpp pack` carries the DLL by default
# while `--mode system` records host-coupled instead of refusing.
#
# Read only when the default toolchain here is the llvm row (703's own
# convention): a runner whose default is msvc@system prints that and asserts
# nothing, because the cl.exe cells are covered by the unit property test
# (NinjaBackendPeRuntime.CrtWordIsOneWordEqualOnCompileAndLink) and by
# 180/181.
set -e

MCPP_HOME="${MCPP_HOME:-$HOME/.mcpp}"
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

OBJDUMP=$(ls "${MCPP_HOME}/registry/data/xpkgs/xim-x-llvm"/*/bin/llvm-objdump.exe 2>/dev/null | head -1)
[[ -x "$OBJDUMP" ]] || OBJDUMP=$(command -v llvm-objdump 2>/dev/null || true)
[[ -x "$OBJDUMP" ]] || OBJDUMP=$(command -v llvm-objdump.exe 2>/dev/null || true)
[[ -x "$OBJDUMP" ]] || OBJDUMP=$(command -v objdump 2>/dev/null || true)
[[ -x "$OBJDUMP" ]] || OBJDUMP=$(command -v objdump.exe 2>/dev/null || true)
imports_of() {   # $1 = exe path -- the DLL names on its PE import table
    [[ -x "$OBJDUMP" ]] || return 0
    "$OBJDUMP" -p "$1" 2>/dev/null | grep -i "DLL Name"
}

cd "$TMP"
mkdir -p app/src
cat > app/src/main.cpp <<'CPP'
import std;
int main() { std::println("crt-abi-ok"); return 0; }
CPP

write_app() {   # $1 = extra [build] lines
    cat > app/mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[build]
default-profile = "dev"
$1
TOML
}

record_of() {   # prints the distributable contract from the one resolution.json
    tr -d ' \n\r' < "$(find target -name resolution.json -not -path '*/.build-mcpp/*' | head -1)" \
        | grep -o '"distributable":"[a-z-]*"' | head -1 | cut -d'"' -f4
}

write_app ''
cd app
"$MCPP" build > default.log 2>&1 || fail "the default build failed" default.log
if ! grep -q "Resolved llvm@" default.log; then
    echo "READING #718: the default toolchain here is not the llvm row: $(grep -m1 'Resolved' default.log)"
    echo "PASS: 814 the llvm row defaults to the dynamic CRT (not the llvm row; nothing to assert)"
    exit 0
fi

EXE=$(find target -name "app.exe" -path "*/bin/*" | head -1)
[[ -n "$EXE" ]] || fail "no exe produced" default.log
BIN_DIR=$(dirname "$EXE")

imports=$(imports_of "$EXE")
echo "default imports:"; echo "$imports"
echo "$imports" | grep -iq "vcruntime140" \
    || fail "the default LLVM-row program imports no vcruntime140.dll" default.log
[[ -f "$BIN_DIR/vcruntime140.dll" ]] \
    || fail "vcruntime140.dll was not staged beside the exe (found: $(ls "$BIN_DIR"))" default.log

# Runs with the toolset removed from PATH: only the staged copy may serve it.
run_out=$(cd "$BIN_DIR" && PATH="/usr/bin:/c/Windows/System32" ./app.exe 2>&1) \
    || fail "the default program did not run with the VS directories off PATH" <(echo "$run_out")
[[ "$run_out" == *"crt-abi-ok"* ]] || fail "unexpected run output: $run_out"

contract=$(record_of)
echo "READING #718 default record: distributable=$contract"
[ "$contract" = "toolchain-coupled" ] \
    || fail "the llvm row's undeclared default recorded '$contract', not toolchain-coupled" default.log

STD_CACHE="$MCPP_HOME/build-cache/v1/std"
dyn_bmis=$(grep -rl -- '-fms-runtime-lib=dll' "$STD_CACHE" 2>/dev/null | wc -l | tr -d ' ')
[ "${dyn_bmis:-0}" -ge 1 ] \
    || fail "no std BMI command recorded the dynamic CRT word" default.log

echo "ok: the default llvm-row program imports and stages vcruntime140.dll, and runs with a clean PATH"

cd ..
write_app 'cxx_runtime = "self-contained"'
cd app
rm -rf target
"$MCPP" build > self-contained.log 2>&1 || fail "the self-contained build failed" self-contained.log
EXE=$(find target -name "app.exe" -path "*/bin/*" | head -1)
[[ -n "$EXE" ]] || fail "no exe produced (self-contained)" self-contained.log

imports=$(imports_of "$EXE")
echo "self-contained imports:"; echo "$imports"
(echo "$imports" | grep -iqE "vcruntime|msvcp") \
    && { fail "self-contained still imports a CRT DLL" <(echo "$imports"); } || true
[[ -f "$(dirname "$EXE")/vcruntime140.dll" ]] \
    && { fail "self-contained staged vcruntime140.dll, which it should not need" self-contained.log; } || true

contract=$(record_of)
echo "READING #718 self-contained record: distributable=$contract"
[ "$contract" = "self-contained" ] \
    || fail "self-contained recorded '$contract'" self-contained.log

static_bmis=$(grep -rl -- '-fms-runtime-lib=static' "$STD_CACHE" 2>/dev/null | wc -l | tr -d ' ')
[ "${static_bmis:-0}" -ge 1 ] \
    || fail "no std BMI command recorded the static CRT word" self-contained.log

echo "ok: self-contained restores the static CRT and imports no vcruntime/msvcp DLL"

# Switching back (A, B, A): the dynamic-CRT identity must still be valid and
# reusable, not clobbered by the intervening static build.
cd ..
write_app ''
cd app
rm -rf target
"$MCPP" build > default-again.log 2>&1 || fail "the second default build failed" default-again.log
EXE=$(find target -name "app.exe" -path "*/bin/*" | head -1)
imports=$(imports_of "$EXE")
echo "$imports" | grep -iq "vcruntime140" \
    || fail "switching back to the default lost the dynamic CRT" default-again.log
echo "ok: switching A, B, A keeps both std BMI identities valid"

# `mcpp pack`: the default mode carries the DLL. An explicit `--mode system`
# on a DEFAULTED (never-declared) `toolchain-coupled` contract outranks the
# default and resolves it to host-coupled instead of refusing — the
# contradiction stays reserved for an EXPLICIT `toolchain-coupled` (unchanged,
# and not this fixture's row: it never wrote `cxx_runtime` down).
#
# `resolution.json` is a property of the BUILD, not of one `pack` invocation,
# so the observable here is the package's own contents: `--format dir` avoids
# needing an unzip step to look inside.
MARKER="$TMP/marker"; touch "$MARKER"
pack_out=$("$MCPP" pack 2>&1) || fail "the default pack failed" <(echo "$pack_out")
DIST=$(find target/dist -maxdepth 1 -mindepth 1 -newer "$MARKER" | head -1)
[[ -n "$DIST" ]] || fail "no pack output produced" <(echo "$pack_out"; find target/dist)
{ [[ -d "$DIST" ]] && find "$DIST" -iname "vcruntime140.dll" | grep -q .; } \
    || unzip -l "$DIST" 2>/dev/null | grep -qi "vcruntime140.dll" \
    || fail "the default pack did not carry vcruntime140.dll" <(echo "$pack_out"; echo "$DIST")

touch "$MARKER"
system_out=$("$MCPP" pack --mode system --format dir 2>&1) \
    || fail "--mode system refused a defaulted (never-declared) toolchain-coupled contract" \
        <(echo "$system_out")
SYSTEM_DIST=$(find target/dist -maxdepth 1 -mindepth 1 -newer "$MARKER" | head -1)
[[ -n "$SYSTEM_DIST" ]] || fail "--mode system produced no output" <(echo "$system_out")
find "$SYSTEM_DIST" -iname "vcruntime140.dll" | grep -q . \
    && { fail "--mode system still bundled vcruntime140.dll for a defaulted (host-coupled) contract" \
        <(echo "$system_out"; find "$SYSTEM_DIST"); } || true

echo "ok: mcpp pack carries the DLL by default, and --mode system resolves the defaulted contract to host-coupled instead of refusing"

# A dependency's `[build] cxxflags` reach its own units after the graph's
# flags, so a CRT word there would compile them against the other CRT: one
# image, two CRTs. A contradicting word is refused, naming the dependency;
# an agreeing one is accepted without a warning (`cxx_runtime` is the root's).
cd "$TMP"
mkdir -p dep/src
printf 'export module dep;\nexport int dep_value() { return 7; }\n' > dep/src/dep.cppm
write_dep() {   # $1 = the dependency's cxxflags word
    cat > dep/mcpp.toml <<TOML
[package]
name    = "dep"
version = "0.1.0"

[targets.dep]
kind = "lib"

[build]
cxxflags = ["$1"]
TOML
}
mkdir -p user/src
printf 'import dep;\nint main() { return dep_value() == 7 ? 0 : 1; }\n' > user/src/main.cpp
cat > user/mcpp.toml <<'TOML'
[package]
name    = "user"
version = "0.1.0"

[dependencies]
dep = { path = "../dep" }
TOML
cd user
write_dep_here() { (cd .. && write_dep "$1"); }
write_dep_here "-fms-runtime-lib=static"
if "$MCPP" build > dep-static.log 2>&1; then
    fail "a dependency's contradicting CRT word was not refused" dep-static.log
fi
grep -q "dependency 'dep'" dep-static.log && grep -q -- "-fms-runtime-lib=static" dep-static.log \
    || fail "the refusal does not name the dependency and the word" dep-static.log
write_dep_here "-fms-runtime-lib=dll"
"$MCPP" build > dep-dll.log 2>&1 || fail "a dependency's agreeing CRT word was refused" dep-dll.log
if grep -q "agrees with the CRT model" dep-dll.log; then
    fail "a dependency's agreeing CRT word was warned" dep-dll.log
fi
echo "ok: a dependency's contradicting CRT word is refused by name; an agreeing one is accepted silently"

echo "PASS: 814 the llvm row defaults to the dynamic CRT"
