#!/usr/bin/env bash
# requires: windows
# 794_a_windows_program_finds_a_dll_through_runtime_search_dir.sh — design
# §5.4 R1' on native Windows (SPEC-007 R4.1, R4.3): PE has no run path, so
# `mcpp::runtime_search_dir(dir)` reaches the program two ways — `mcpp run`
# through `PATH`, and `mcpp pack` by placing the DLL beside the packed
# executable. 795 carries the same three checks on this host through
# mingw-cross + wine and is the one that gives this round a passing reading
# before release; this script is the native leg the design and SPEC-007 R8.1
# both ask for (a criterion on every platform a feature claims), run on
# Windows CI.
#
# THE DLL IS DISCOVERED, NOT DECLARED AS A DEPENDENCY. `mathkit` is built
# first, on its own, and its `.dll`/import library are then handed to `app`'s
# build.mcpp as plain files it locates itself (`link_lib`/`link_search`/
# `runtime_search_dir`) — the vcpkg/Qt shape the directive exists for — not
# through `[dependencies]`, so this does not exercise mcpp's own
# `kind = "shared"` dependency-closure mechanism (covered by 242/255/256) and
# cannot pass by accident through it.
#
# Pinned to the mingw payload (not whatever native toolchain autodetects) for
# a deterministic `libmathkit.dll` / `libmathkit.dll.a` naming pair, the same
# choice 97_mingw_toolchain.sh makes for the same reason.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p mathkit/src
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
export namespace mk { int answer(); }
EOF
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk { int answer() { return 42; } }
extern "C" __declspec(dllexport) int mk_answer_extern() { return mk::answer(); }
EOF
cat > mathkit/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[toolchain]
windows = "gcc@16.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "shared"
EOF

MCPP="${MCPP:-mcpp}"
( cd mathkit && "$MCPP" build > build.log 2>&1 ) || {
    cat mathkit/build.log; echo "FAIL: mathkit build failed"; exit 1; }
DLL="$(find mathkit/target -name 'libmathkit.dll' | head -1)"
IMP="$(find mathkit/target -name 'libmathkit.dll.a' | head -1)"
[[ -n "$DLL" && -n "$IMP" ]] || { find mathkit/target -type f; echo "FAIL: mathkit did not produce a DLL + import library"; exit 1; }
DLLDIR_HOST="$(host_path "$(cd "$(dirname "$DLL")" && pwd)")"
LIBDIR_HOST="$(host_path "$(cd "$(dirname "$IMP")" && pwd)")"

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
extern "C" int mk_answer_extern();
int main() { return mk_answer_extern() == 42 ? 0 : 1; }
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[toolchain]
windows = "gcc@16.1.0"
[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
cat > app/build.mcpp <<EOF
import mcpp;
int main() {
    // mcpp links executables in static mode, which leaves the linker
    // refusing an import library. The dynamic-mode switch only works
    // immediately before the -l it enables (docs/12), so the search path and
    // the switch are both spelled through link_flag, in order.
    mcpp::link_flag("-L$LIBDIR_HOST");
    mcpp::link_flag("-Wl,-Bdynamic");
    mcpp::link_lib("mathkit");
    mcpp::runtime_search_dir("$DLLDIR_HOST");
    return 0;
}
EOF

cd app
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "FAIL: app build failed"; exit 1; }
EXE="$(find target -name 'app.exe' | head -1)"
[[ -n "$EXE" ]] || { echo "FAIL: no app.exe produced"; exit 1; }

# ── 1. `mcpp run` finds the library through PATH (R4.3) ─────────────────────
rc=0; out="$("$MCPP" run 2>&1)" || rc=$?
[[ $rc -eq 0 ]] || { echo "FAIL: mcpp run failed (rc=$rc): $out"; exit 1; }

# ── 2. `mcpp pack` places the DLL beside the packed executable ──────────────
"$MCPP" pack --format dir > pack.log 2>&1 || { cat pack.log; echo "FAIL: pack failed"; exit 1; }
PACKED_EXE="$(find target/dist -name 'app.exe' | head -1)"
[[ -n "$PACKED_EXE" ]] || { find target/dist -type f; echo "FAIL: no packed app.exe"; exit 1; }
[[ -f "$(dirname "$PACKED_EXE")/libmathkit.dll" ]] || {
    find "$(dirname "$PACKED_EXE")" -maxdepth 1
    echo "FAIL: mcpp pack did not place the DLL from the runtime search directory beside the exe"
    exit 1; }

# ── 3. the packed program also runs by itself, from its own directory ──────
( cd "$(dirname "$PACKED_EXE")" && "./$(basename "$PACKED_EXE")" ) \
    || { echo "FAIL: the packed program did not run from its own directory"; exit 1; }

echo "PASS: 794_a_windows_program_finds_a_dll_through_runtime_search_dir"
