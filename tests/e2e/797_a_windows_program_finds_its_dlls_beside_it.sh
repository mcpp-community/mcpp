#!/usr/bin/env bash
# requires: mingw-cross wine
# SPEC-007 R4.3: a Windows program's runtime DLLs are placed beside it after
# its link.
#
# A PE image has no run path. A DLL in a runtime search directory therefore
# served `mcpp run`, which puts the directory on PATH, and `mcpp pack`, which
# stages the closure, and not a program started by hand from the build
# directory. The engine now follows the link with an edge that reads the
# program's import closure, as `mcpp pack` does, and publishes each DLL it
# resolves in a runtime search directory beside the program.
#
# The directory here is populated by a `prepare` action, so the DLL does not
# exist when the build program runs and no plan-time listing can name it: the
# first build has to place it after the link. Four properties:
#
#   1. after the FIRST build, the program started from the build directory,
#      with nothing on PATH, finds the DLL (wine is the loader);
#   2. a build with nothing changed neither relinks nor places again;
#   3. a DLL replaced in its directory replaces the copy on the next build;
#   4. no system DLL is ever copied.
#
# WINE IS EVIDENCE, NOT PROOF, as in 257: what it gives is a PE loader actually
# resolving the DLL from the program's directory.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

TRIPLE=x86_64-windows-gnu

fail() { [ -n "$2" ] && cat "$2"; echo "FAIL: $1"; exit 1; }

# ── The DLL, built once per answer, kept outside the consumer ──────────────
mkdir -p mathkit/src store implib
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
export extern "C" int mk_answer();
EOF
cat > mathkit/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "shared"
EOF
build_dll() {
    cat > mathkit/src/impl.cpp <<EOF
module mathkit;
extern "C" int mk_answer() { return $1; }
EOF
    (cd mathkit && "$MCPP" build --target "$TRIPLE" > build.log 2>&1) \
        || fail "the DLL did not build" mathkit/build.log
    cp "$(find mathkit/target -name libmathkit.dll | head -1)" store/libmathkit.dll
    cp "$(find mathkit/target -name libmathkit.dll.a | head -1)" implib/libmathkit.dll.a
}
build_dll 42

# ── The consumer: a prepare action fills rt/, declared a runtime search dir ─
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
extern "C" int mk_answer();
int main() { std::printf("ANSWER %d\n", mk_answer()); return 0; }
EOF
IMPLIB="$(host_path "$TMP/implib/libmathkit.dll.a")"
cat > app/mcpp.toml <<EOF
[package]
name    = "dllapp"
version = "0.1.0"
[build]
ldflags = ["$IMPLIB"]
EOF
STORE="$(host_path "$TMP/store/libmathkit.dll")"
RT="$(host_path "$TMP/rt")"
cat > app/build.mcpp <<EOF
import std;
import mcpp;
int main() {
    mcpp::runtime_search_dir("$RT");
    mcpp::action a;
    a.id          = "fill-rt";
    a.role        = mcpp::roles::prepare;
    a.description = "an installation whose file names the program does not know";
    a.arg("\${mcpp.self}").arg("stage").arg("--output").arg("$RT/libmathkit.dll").arg("$STORE")
     .input("$STORE")
     .output((std::string(mcpp::out_dir()) + "/fill-rt.stamp").c_str())
     .output_dir("$RT")
     .submit();
}
EOF

cd app
bindir() { dirname "$(find target -path "*$TRIPLE*" -name dllapp.exe | head -1)"; }
run_by_hand() { (cd "$(bindir)" && WINEDEBUG=-all wine ./dllapp.exe 2>&1 | tr -d '\r'); }

# ── 1. the first build places the DLL ──────────────────────────────────────
"$MCPP" build --target "$TRIPLE" -v > b1.log 2>&1 || fail "the first build failed" b1.log
[ -f "$(bindir)/libmathkit.dll" ] || { ls "$(bindir)"; fail "libmathkit.dll is not beside the program after the first build" b1.log; }
out="$(run_by_hand)"
printf '%s\n' "$out" | grep -qx 'ANSWER 42' || fail "the program started by hand did not load its DLL: $out"
echo "  ok: after the first build the program started by hand finds its DLL"

# ── 2. nothing changed: no relink, no placement ───────────────────────────
"$MCPP" build --target "$TRIPLE" -v > b2.log 2>&1 || fail "the second build failed" b2.log
"$MCPP" build --target "$TRIPLE" -v > b3.log 2>&1 || fail "the third build failed" b3.log
for log in b2.log b3.log; do
    if grep -q 'place-dlls' "$log"; then fail "a build with nothing changed placed the DLLs again" "$log"; fi
    if grep -q -- '-o bin/dllapp.exe' "$log"; then fail "a build with nothing changed relinked the program" "$log"; fi
done
echo "  ok: a build with nothing changed neither relinks nor places again"

# ── 3. a replaced DLL is placed again ─────────────────────────────────────
cd "$TMP"
build_dll 43
cd app
"$MCPP" build --target "$TRIPLE" -v > b4.log 2>&1 || fail "the build after the DLL changed failed" b4.log
grep -q 'place-dlls' b4.log || fail "the placement did not run after the DLL changed" b4.log
out="$(run_by_hand)"
printf '%s\n' "$out" | grep -qx 'ANSWER 43' || fail "the copy beside the program was not replaced: $out"
echo "  ok: a DLL replaced in its directory replaces the copy"

# ── 4. no system DLL is copied ────────────────────────────────────────────
if ls "$(bindir)" | grep -qiE '^(kernel32|msvcrt|ucrtbase|user32|api-ms-|ext-ms-)'; then
    ls "$(bindir)"
    fail "a system DLL was copied beside the program"
fi
echo "  ok: no system DLL is copied"

echo "PASS: a Windows program finds its runtime DLLs beside it"
