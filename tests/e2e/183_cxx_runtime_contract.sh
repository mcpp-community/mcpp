#!/usr/bin/env bash
# mcpp-community/mcpp#336 — the C++ runtime distribution contract.
#
# Three things this locks down, all of which were broken or absent before:
#
#   1. A global object that uses std::cout during static initialization must
#      run. On macOS with the default (self-contained → static libc++) it
#      SIGSEGV'd at process start: Mach-O runs __init_offsets in link order and
#      has no priority-ordered init section, so the stream initializer pulled
#      out of libc++.a landed last and `std::cout`'s vptr was still zero.
#      libc++'s <iostream> carries no `ios_base::Init` guard of its own — the
#      remedy the standard provides and libstdc++/MSVC STL use — so no amount
#      of package-side code could fix it either. This is the repro from the
#      issue, verbatim.
#
#   2. `cxx_runtime = "host-coupled"` (and its older spelling
#      `static_stdlib = false`) must reach TEST binaries. It silently did not
#      from 0.0.86 to 2026.8.2.2 while the documentation kept promising it,
#      because the test link path was a second, ungated derivation of the same
#      decision.
#
#   3. The contract is a claim about the artifact's runtime dependency set, so
#      it is checked against the artifact, not against the flags we passed.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

case "$(uname -s)" in
    Darwin)               HOST=macos   ;;
    MINGW*|MSYS*|CYGWIN*) HOST=windows ;;
    *)                    HOST=linux   ;;
esac

# ── the repro ──────────────────────────────────────────────────────────────
mkdir -p siof/src siof/tests
cat > siof/mcpp.toml <<'EOF'
[package]
name    = "siof"
version = "0.1.0"
EOF

# `std::cout.rdbuf()` during static init. Touching the stream is the point:
# merely taking its address would not fault.
cat > siof/src/main.cpp <<'EOF'
#include <iostream>
struct Early {
    Early() { probe = std::cout.rdbuf(); }
    std::streambuf* probe = nullptr;
};
static Early early_user;
int main() {
    std::cout << "static-init-ok\n";
    return early_user.probe == nullptr;
}
EOF

# The same hazard in a test binary — the role whose opt-out was unreachable.
cat > siof/tests/early.cpp <<'EOF'
#include <iostream>
struct Early {
    Early() { probe = std::cout.rdbuf(); }
    std::streambuf* probe = nullptr;
};
static Early early_user;
int main() {
    std::cout << "static-init-ok\n";
    return early_user.probe == nullptr;
}
EOF

cd siof

echo "== 1. default (self-contained): the repro must run =="
"$MCPP" build >/dev/null 2>&1 || { echo "FAIL: default build"; exit 1; }
run_out=$("$MCPP" run 2>&1) || {
    echo "FAIL: default-profile binary crashed during static initialization"
    echo "$run_out"
    echo "(macOS: this is #336 — the libc++ stream initializer ordered last"
    echo " in __init_offsets. Check that the mcpp_ios_init object is the FIRST"
    echo " input of the link edge in target/*/*/build.ninja.)"
    exit 1; }
echo "$run_out" | grep -q "static-init-ok" || {
    echo "FAIL: expected static-init-ok, got: $run_out"; exit 1; }

echo "== 2. the same, as a test binary =="
test_out=$("$MCPP" test 2>&1) || {
    echo "FAIL: test binary crashed during static initialization"
    echo "$test_out"; exit 1; }
echo "$test_out" | grep -q "test result ok" || {
    echo "FAIL: unexpected test output: $test_out"; exit 1; }

# On macOS the shim is what makes 1 and 2 pass, so assert it is actually
# there and actually first — a silently-absent shim would leave this test
# passing by luck on a future toolchain and failing for users on another.
if [[ "$HOST" == macos ]]; then
    NINJA=$(find target -name build.ninja | head -1)
    grep -q "mcpp_ios_init" "$NINJA" || {
        echo "FAIL: no initializer-ordering shim in the macOS link"; exit 1; }
    # `build <exe> : cxx_link <first-input> ...`
    link_line=$(grep -E "^build .*: cxx_link " "$NINJA" | head -1)
    first_in=$(echo "$link_line" | sed -E 's/^build .*: cxx_link +([^ ]+).*/\1/')
    case "$first_in" in
        *mcpp_ios_init*) ;;
        *) echo "FAIL: shim object is not the first link input: $link_line"
           exit 1 ;;
    esac
fi

echo "== 3. cxx_runtime = host-coupled reaches BOTH roles (#336 part one) =="
# Capture the self-contained shape first so the comparison is against this
# machine's actual output rather than a hard-coded flag string.
NINJA=$(find target -name build.ninja | head -1)
self_units=$(grep -c "unit_ldflags" "$NINJA" || true)

cat >> mcpp.toml <<'EOF'

[build]
cxx_runtime = "host-coupled"
EOF
"$MCPP" build >/dev/null 2>&1 || { echo "FAIL: host-coupled build"; exit 1; }
"$MCPP" test  >/dev/null 2>&1 || { echo "FAIL: host-coupled test"; exit 1; }

NINJA=$(find target -name build.ninja | head -1)
# Whatever the platform's self-contained mechanism is (-static-libstdc++,
# -static, -load_hidden archives), host-coupled must not carry it — for the
# test target as much as for the binary.
if grep -E "unit_ldflags.*(-static-libstdc\+\+|-load_hidden|-nostdlib\+\+)" "$NINJA" >/dev/null; then
    echo "FAIL: host-coupled still emits a self-contained mechanism:"
    grep "unit_ldflags" "$NINJA"
    exit 1
fi

echo "== 4. the older spelling means the same thing =="
"$MCPP" build >/dev/null 2>&1
sed 's/cxx_runtime = "host-coupled"/static_stdlib = false/' mcpp.toml > mcpp.toml.tmp
mv mcpp.toml.tmp mcpp.toml
"$MCPP" build >/dev/null 2>&1 || { echo "FAIL: static_stdlib=false build"; exit 1; }
NINJA=$(find target -name build.ninja | head -1)
if grep -E "unit_ldflags.*(-static-libstdc\+\+|-load_hidden|-nostdlib\+\+)" "$NINJA" >/dev/null; then
    echo "FAIL: static_stdlib = false is not an alias of host-coupled:"
    grep "unit_ldflags" "$NINJA"
    exit 1
fi

echo "== 5. an invalid contract is rejected at parse time =="
sed 's/static_stdlib = false/cxx_runtime = "static"/' mcpp.toml > mcpp.toml.tmp
mv mcpp.toml.tmp mcpp.toml
set +e
bad_out=$("$MCPP" build 2>&1)
bad_rc=$?
set -e
[[ $bad_rc -ne 0 ]] || { echo "FAIL: invalid cxx_runtime accepted"; exit 1; }
echo "$bad_out" | grep -q "self-contained" || {
    echo "FAIL: the error does not list the valid values: $bad_out"; exit 1; }

echo "== 6. self-contained is checked against the ARTIFACT =="
# The contract is a claim about the runtime dependency set. Assert it there,
# not on the flags — a mechanism that stops working would otherwise keep
# reporting success. (The per-OS allowed set is deliberately narrow.)
cd "$TMP"
mkdir -p dist/src
cat > dist/mcpp.toml <<'EOF'
[package]
name    = "dist"
version = "0.1.0"
EOF
cat > dist/src/main.cpp <<'EOF'
#include <iostream>
int main() { std::cout << "hi\n"; }
EOF
cd dist
"$MCPP" build >/dev/null 2>&1 || { echo "FAIL: dist build"; exit 1; }

case "$HOST" in
  macos)
    BIN=$(find target -path "*/bin/dist" -type f | head -1)
    [[ -n "$BIN" ]] || { echo "FAIL: no binary"; exit 1; }
    deps=$(otool -L "$BIN" | tail -n +2 | awk '{print $1}')
    # A self-contained macOS artifact links libSystem and nothing else from
    # /usr/lib — in particular NOT /usr/lib/libc++.1.dylib, which is what
    # would pin it to the build machine's OS version.
    if echo "$deps" | grep -q "libc++"; then
        echo "FAIL: self-contained artifact still depends on a libc++ dylib:"
        echo "$deps"; exit 1
    fi
    ;;
  linux)
    BIN=$(find target -path "*/bin/dist" -type f | head -1)
    [[ -n "$BIN" ]] || { echo "FAIL: no binary"; exit 1; }
    if command -v readelf >/dev/null 2>&1; then
        needed=$(readelf -d "$BIN" 2>/dev/null | grep NEEDED || true)
        # libstdc++/libc++ must not be there; libc/libm/loader may be.
        if echo "$needed" | grep -Eq "libstdc\+\+|libc\+\+"; then
            echo "FAIL: self-contained artifact still depends on a C++ runtime:"
            echo "$needed"; exit 1
        fi
    else
        echo "  (readelf unavailable — dependency-set assertion skipped)"
    fi
    ;;
  windows)
    # PE has no rpath and no ldd; the equivalent check is that the exe runs
    # with the toolchain off PATH. e2e 182 already owns that assertion for
    # the MinGW leg, so this one stays on the flag contract.
    echo "  (windows: dependency-set assertion covered by e2e 182)"
    ;;
esac

echo "PASS: cxx runtime contract"
