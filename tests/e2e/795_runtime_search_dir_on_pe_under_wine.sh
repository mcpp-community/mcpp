#!/usr/bin/env bash
# requires: mingw-cross wine
# 795_runtime_search_dir_on_pe_under_wine.sh — the Linux-hosted stand-in for
# 794's native-Windows leg of design §5.4 R1' / SPEC-007 R4.1, R4.3: on a PE
# target, `mcpp::runtime_search_dir(dir)` is what `mcpp run` uses (through
# `PATH`, since PE has no run path) and what `mcpp pack` uses (placing the
# DLL beside the packed executable). 794 carries the same three checks on a
# real Windows runner and cannot be exercised there ahead of time; this
# script exercises the identical property under wine, which is real evidence
# for the loader actually resolving the DLL (see 257's own header for that
# caveat) and gives this round a PASSING reading before release, which 794
# alone cannot.
#
# THE DLL IS DISCOVERED, NOT DECLARED AS A DEPENDENCY. `mathkit` is built
# first and its `.dll`/import library are then handed to `app`'s build.mcpp
# as plain files it locates itself (`link_lib`/`link_search`/
# `runtime_search_dir`) — the vcpkg/Qt shape the directive exists for — so
# this does not exercise mcpp's OWN `kind = "shared"` dependency-closure
# mechanism (already covered by 257) and cannot pass by accident through it.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
cleanup() {
    local status=$?
    if [[ -n "${WINEPREFIX:-}" ]] && command -v wineserver >/dev/null 2>&1; then
        wineserver -k >/dev/null 2>&1 || true
        wineserver -w >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP" 2>/dev/null || true
    return $status
}
trap cleanup EXIT
cd "$TMP"

TRIPLE=x86_64-windows-gnu

# ── the prebuilt-plugin shape: a DLL + import library, built once. A plain
#    `extern "C"` entry point sidesteps guessing a C++ mangled name across
#    the import library from the consumer below. ───────────────────────────
mkdir -p mathkit/src
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
export namespace mk { int answer(); }
EOF
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk { int answer() { return 42; } }
extern "C" int mk_answer_extern() { return mk::answer(); }
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

MCPP="${MCPP:-mcpp}"
( cd mathkit && "$MCPP" build --target "$TRIPLE" > build.log 2>&1 ) || {
    cat mathkit/build.log; echo "FAIL: mathkit build failed"; exit 1; }
DLL="$(find mathkit/target -name 'libmathkit.dll' | head -1)"
IMP="$(find mathkit/target -name 'libmathkit.dll.a' | head -1)"
[[ -n "$DLL" && -n "$IMP" ]] || { echo "FAIL: mathkit did not produce a DLL + import library"; exit 1; }
DLLDIR=$(realpath "$(dirname "$DLL")")
LIBDIR=$(realpath "$(dirname "$IMP")")

# ── the consumer: NO `[dependencies]` entry for mathkit at all ─────────────
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
extern "C" int mk_answer_extern();
int main() { return mk_answer_extern() == 42 ? 0 : 1; }
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind = "bin"
main = "src/main.cpp"
[target.x86_64-windows-gnu]
runner = ["wine"]
EOF
cat > app/build.mcpp <<EOF
import mcpp;
int main() {
    // PE/MinGW: mcpp links executables in static mode, which leaves the
    // linker refusing an import library. The dynamic-mode switch only works
    // immediately before the -l it enables (docs/12), so the search path and
    // the switch are both spelled through link_flag, in order.
    mcpp::link_flag("-L$(host_path "$LIBDIR")");
    mcpp::link_flag("-Wl,-Bdynamic");
    mcpp::link_lib("mathkit");
    mcpp::runtime_search_dir("$(host_path "$DLLDIR")");
    return 0;
}
EOF

cd app
"$MCPP" build --target "$TRIPLE" > build.log 2>&1 || { cat build.log; echo "FAIL: app build failed"; exit 1; }
EXE="$(find target/$TRIPLE -name 'app.exe' | head -1)"
[[ -n "$EXE" ]] || { echo "FAIL: no app.exe produced"; exit 1; }

# NOTE: the DLL is typically ALREADY beside the exe at this point. mathkit's
# directory exists (with the DLL already in it) at CONFIGURE time -- app's
# build.mcpp runs against an already-built mathkit, the vcpkg/Qt shape where
# the prefix is installed before the build -- and mcpp's existing plan-time
# scan of every `runtimeSearchDirs` entry for `.dll` files (plan.cppm) already
# deploys what it finds there, on every platform, independently of this PR.
# That is not what this script is for: it is for `runtime_search_dir` FEEDING
# that scan and `mcpp run`/`mcpp pack` correctly, which is exactly R1'. A
# directory a `prepare` action populates DURING the build -- empty at plan
# time -- is the case the engine's post-link placement (design §5.4 W) exists
# for, and is out of this item's scope (W is a separate task).

# ── 1. `mcpp run` finds the library through PATH (R4.3), under wine ────────
export WINEPREFIX="$TMP/wineprefix" WINEDEBUG=-all
rc=0; out="$("$MCPP" run --target "$TRIPLE" 2>&1)" || rc=$?
[[ $rc -eq 0 ]] || { echo "FAIL: mcpp run failed (rc=$rc): $out"; exit 1; }

# ── 2. `mcpp pack` places the DLL beside the packed executable ─────────────
"$MCPP" pack --target "$TRIPLE" --format dir > pack.log 2>&1 || { cat pack.log; echo "FAIL: pack failed"; exit 1; }
PACKED_EXE="$(find target/dist -name 'app.exe' | head -1)"
[[ -n "$PACKED_EXE" ]] || { find target/dist; echo "FAIL: no packed app.exe"; exit 1; }
[[ -f "$(dirname "$PACKED_EXE")/libmathkit.dll" ]] || {
    find "$(dirname "$PACKED_EXE")" -maxdepth 1
    echo "FAIL: mcpp pack did not place the DLL from the runtime search directory beside the exe"
    exit 1; }

# ── the packed program also runs stand-alone under wine (belt and braces) ──
rc2=0; out2="$(wine "$PACKED_EXE" 2>/dev/null)" || rc2=$?
[[ $rc2 -eq 0 ]] || { echo "FAIL: the packed program did not run cleanly under wine (rc=$rc2)"; exit 1; }

echo "PASS: 795_runtime_search_dir_on_pe_under_wine"
