#!/usr/bin/env bash
# requires: elf
# A link unit with no C++ in it is linked by the C driver (#426).
#
# Every link went through `$cxx`. `g++` appends `-lstdc++` unconditionally, and
# mcpp uses `--as-needed` in exactly one place (`-latomic`, flags.cppm), so a
# PURE C shared library came out depending on the C++ runtime:
#
#   $ readelf -d bin/libpurec.so | grep NEEDED
#     libstdc++.so.6   ← a library with no C++ in it
#     libm.so.6
#     libgcc_s.so.1
#     libc.so.6
#
# Measured against the C driver on the same object with the same ldflags:
# `libc.so.6`, and nothing else. All three extra entries came from the driver;
# `nm -D --undefined-only` shows the only C++-looking undefined symbol is
# `__cxa_finalize@GLIBC_2.2.5`, which is glibc's, not libstdc++'s.
#
# Nobody's build breaks from this — it is a distribution defect. Anyone
# deploying that .so has to ship a C++ runtime with it, and its runtime closure
# is larger than the code justifies.
#
# ⚠️ BOTH DIRECTIONS. Checking only that libstdc++ disappeared would pass an
# implementation that linked EVERYTHING with the C driver, which turns every
# C++ target into a pile of undefined `_ZSt…`. So a C++ target must still link,
# run, and use the C++ rule.
set -e

_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -z "${MCPP:-}" ]; then
  MCPP="$(bash "$_root/.github/tools/newest_artifact.sh" "$_root" mcpp 2>/dev/null || true)"
  [ -n "$MCPP" ] || { echo "SKIP: no mcpp binary built yet"; exit 0; }
fi
case "$MCPP" in /*) ;; *) MCPP="$_root/$MCPP" ;; esac
[ -x "$MCPP" ] || { echo "FAIL: MCPP=$MCPP is not executable"; exit 1; }

TMP=$(mktemp -d)
trap "rm -rf $TMP || true" EXIT

# ⚠️ THREE SEPARATE PROJECTS, not three targets in one. Targets in a single
# package share that package's objects — a `[targets.X] sources` list does not
# fence them off — so a C++ target next door would put its object into the
# pure-C library's link and the predicate under test would never see a C-only
# unit at all. Discovered by writing it the other way first.
tc_block() {
  printf '[toolchain]\ndefault = "gcc@16.1.0"\nmacos   = "llvm@22.1.8"\nwindows = "llvm@20.1.7"\n'
}

# ── 1. a pure-C shared library ─────────────────────────────────────────────
mkdir -p "$TMP/purec/src"
{ printf '[package]\nname = "purec"\nversion = "0.1.0"\n\n'; tc_block
  printf '\n[targets.purec]\nkind = "shared"\n'; } > "$TMP/purec/mcpp.toml"
cat > "$TMP/purec/src/purec.c" <<'EOF'
int purec_add(int a, int b) { return a + b; }
EOF

echo "== 1. pure C =="
( cd "$TMP/purec" && "$MCPP" build --release ) > "$TMP/purec.log" 2>&1 || {
    echo "FAIL: the pure-C project did not build"; tail -30 "$TMP/purec.log"; exit 1; }

NINJA="$(find "$TMP/purec/target" -name build.ninja | head -1)"
grep -qE '^build [^:]*libpurec\.so[^:]*: c_shared ' "$NINJA" || {
    echo "FAIL: the pure-C shared library is not linked by the C rule:"
    grep -E '^build [^:]*libpurec\.so' "$NINJA" | sed 's/^/      /'
    exit 1; }

SO="$(find "$TMP/purec/target" -name 'libpurec.so*' -type f | head -1)"
[ -n "$SO" ] || { echo "FAIL: no libpurec.so produced"; exit 1; }
for lib in libstdc++.so.6 libm.so.6 libgcc_s.so.1; do
    if readelf -d "$SO" | grep -q "\[$lib\]"; then
        echo "FAIL: the pure-C library still depends on $lib"
        readelf -d "$SO" | grep NEEDED | sed 's/^/      /'
        exit 1
    fi
done
readelf -d "$SO" | grep -q '\[libc.so.6\]' || {
    echo "FAIL: the pure-C library lost its dependency on libc itself —"
    echo "      the C driver was not given the link flags it needs."
    readelf -d "$SO" | grep NEEDED | sed 's/^/      /'
    exit 1; }
# ...and it still WORKS. Fewer NEEDED entries is only an improvement if the
# artifact still resolves and runs.
nm -D --defined-only "$SO" | grep -q ' T purec_add' || {
    echo "FAIL: purec_add is no longer exported"; exit 1; }
echo "  c_shared, NEEDED = libc only, symbol exported"

# ── 2. one C++ TU keeps the whole unit on the C++ driver ───────────────────
mkdir -p "$TMP/mixed/src"
{ printf '[package]\nname = "mixed"\nversion = "0.1.0"\n\n'; tc_block
  printf '\n[targets.mixed]\nkind = "shared"\n'; } > "$TMP/mixed/mcpp.toml"
cat > "$TMP/mixed/src/mixed.c" <<'EOF'
int mixed_c(int a) { return a * 2; }
EOF
cat > "$TMP/mixed/src/mixed_cxx.cpp" <<'EOF'
#include <string>
extern "C" int mixed_cxx_len(const char* s) {
    return static_cast<int>(std::string(s).size());
}
EOF

echo "== 2. mixed C/C++ =="
( cd "$TMP/mixed" && "$MCPP" build --release ) > "$TMP/mixed.log" 2>&1 || {
    echo "FAIL: the mixed project did not build"; tail -30 "$TMP/mixed.log"; exit 1; }
NINJA="$(find "$TMP/mixed/target" -name build.ninja | head -1)"
grep -qE '^build [^:]*libmixed\.so[^:]*: cxx_shared ' "$NINJA" || {
    echo "FAIL: a unit containing a C++ translation unit was linked by the C"
    echo "      driver. One C++ object decides the unit, and this direction"
    echo "      fails as undefined symbols rather than as a fat binary."
    grep -E '^build [^:]*libmixed\.so' "$NINJA" | sed 's/^/      /'
    exit 1; }
echo "  cxx_shared"

# ── 3. a C++ binary is untouched, and runs ─────────────────────────────────
mkdir -p "$TMP/cxx/src"
{ printf '[package]\nname = "cxxbin"\nversion = "0.1.0"\n\n'; tc_block; } > "$TMP/cxx/mcpp.toml"
cat > "$TMP/cxx/src/main.cpp" <<'EOF'
import std;
int main() { std::println("cdriver-ok"); return 0; }
EOF

echo "== 3. C++ binary =="
( cd "$TMP/cxx" && "$MCPP" build --release ) > "$TMP/cxx.log" 2>&1 || {
    echo "FAIL: the C++ project did not build"; tail -30 "$TMP/cxx.log"; exit 1; }
NINJA="$(find "$TMP/cxx/target" -name build.ninja | head -1)"
grep -qE '^build [^:]*bin/cxxbin[^:]*: cxx_link ' "$NINJA" || {
    echo "FAIL: the C++ binary is no longer linked by the C++ rule"
    grep -E '^build [^:]*bin/cxxbin' "$NINJA" | sed 's/^/      /'
    exit 1; }
BIN="$(find "$TMP/cxx/target" -path '*/bin/cxxbin' -type f | head -1)"
OUT="$("$BIN")" || { echo "FAIL: the C++ binary exited $?"; exit 1; }
[ "$OUT" = "cdriver-ok" ] || { echo "FAIL: binary printed '$OUT'"; exit 1; }
echo "  cxx_link, runs"

# ── 4. c_ldflags is always defined ─────────────────────────────────────────
# `c_link`/`c_shared` reference `$c_ldflags`. Defining it only when it differs
# from `$ldflags` would make an EMPTY link line the failure mode on exactly the
# toolchains where the two happen to agree — a link that succeeds and produces
# something unrunnable.
grep -q '^c_ldflags ' "$NINJA" || {
    echo "FAIL: c_ldflags is not defined, but the C rules reference it"; exit 1; }
echo "  c_ldflags: defined even where it equals ldflags"

echo "238 C-only link units use the C driver OK"
