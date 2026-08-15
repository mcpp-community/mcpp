#!/usr/bin/env bash
# requires: elf
# `obj/std.o` is linked into a unit only when that unit actually needs it (#416).
#
# `std.o` holds the `std` module's global initialiser (exactly one symbol,
# `_ZGIW3std` — measured). It used to be appended to EVERY Binary, TestBinary
# and SharedLibrary whenever the toolchain merely HAD a prebuilt std module,
# regardless of whether anything in that unit imported it.
#
# ⚠️ THE TEST THAT MATTERS IS THE TRANSITIVE ONE. A unit that never writes
# `import std` itself still needs the initialiser when a module it imports does.
# Checking only "does this unit's own source say import std" is the same
# "the edge exists but nobody depends on it" mistake as #405, and it fails in
# the direction that breaks builds. Both directions are asserted here:
#
#   1. a project with NO std anywhere        → std.o must NOT be linked
#   2. a project that reaches std INDIRECTLY → std.o must be linked, and the
#                                              binary must actually run
#
# The failure mode of getting (2) wrong is loud (undefined `_ZGIW3std` at link
# time), which is why (1) is the one that could regress quietly.
set -e

_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -z "${MCPP:-}" ]; then
  MCPP="$(bash "$_root/.github/tools/newest_artifact.sh" "$_root" mcpp 2>/dev/null || true)"
  [ -n "$MCPP" ] || { echo "SKIP: no mcpp binary built yet"; exit 0; }
fi
case "$MCPP" in /*) ;; *) MCPP="$_root/$MCPP" ;; esac
[ -x "$MCPP" ] || { echo "FAIL: MCPP=$MCPP is not executable"; exit 1; }
export MCPP

TMP=$(mktemp -d)
trap "rm -rf $TMP || true" EXIT

toolchain_block() {
  printf '[toolchain]\ndefault = "gcc@16.1.0"\nmacos   = "llvm@22.1.8"\nwindows = "llvm@20.1.7"\n'
}

# ── 1. nothing imports std anywhere ────────────────────────────────────────
mkdir -p "$TMP/nostd/src"
{ printf '[package]\nname = "nostd"\nversion = "0.1.0"\n'; toolchain_block; } > "$TMP/nostd/mcpp.toml"
cat > "$TMP/nostd/src/x.cppm" <<'EOF'
export module nostd.x;
export int v() { return 3; }
EOF
cat > "$TMP/nostd/src/main.cpp" <<'EOF'
import nostd.x;
int main() { return v() - 3; }
EOF

( cd "$TMP/nostd" && "$MCPP" build --release ) > "$TMP/nostd.log" 2>&1 || {
    echo "FAIL: the no-std project did not build"; tail -30 "$TMP/nostd.log"; exit 1; }

NINJA="$(find "$TMP/nostd/target" -name build.ninja | head -1)"
[ -n "$NINJA" ] || { echo "FAIL: no build.ninja produced"; exit 1; }
if grep -q 'obj/std\.o' "$NINJA"; then
    echo "FAIL: std.o is referenced by a project that imports std nowhere."
    echo "      That object carries the std module's global initialiser; linking"
    echo "      it into a unit with no std at all is what made a pure-C compat"
    echo "      package carry one (#416)."
    grep -n 'obj/std\.o' "$NINJA" | head -5
    exit 1
fi
echo "  no-std project: std.o absent from the graph"

# ── 2. std is reached INDIRECTLY, through one module in between ─────────────
mkdir -p "$TMP/trans/src"
{ printf '[package]\nname = "trans"\nversion = "0.1.0"\n'; toolchain_block; } > "$TMP/trans/mcpp.toml"
# a imports std; b imports a; main imports b. Nothing but `a` mentions std.
cat > "$TMP/trans/src/a.cppm" <<'EOF'
export module trans.a;
import std;
export std::string greet() { return "hi"; }
EOF
cat > "$TMP/trans/src/b.cppm" <<'EOF'
export module trans.b;
import trans.a;
export int n() { return static_cast<int>(greet().size()); }
EOF
cat > "$TMP/trans/src/main.cpp" <<'EOF'
import trans.b;
int main() { return n() - 2; }
EOF

( cd "$TMP/trans" && "$MCPP" build --release ) > "$TMP/trans.log" 2>&1 || {
    echo "FAIL: the transitive project did not build."
    echo "      An undefined \`_ZGIW3std\` here means the reachability walk did"
    echo "      not follow the import edge a <- b <- main."
    tail -30 "$TMP/trans.log"; exit 1; }

NINJA="$(find "$TMP/trans/target" -name build.ninja | head -1)"
grep -q 'obj/std\.o' "$NINJA" || {
    echo "FAIL: std.o is NOT in the graph, but this project reaches std through"
    echo "      trans.b -> trans.a -> std. The predicate is not transitive."
    exit 1; }
echo "  transitive project: std.o present"

# Running it is what proves the initialiser is not merely referenced but works:
# `greet()` returns a std::string, so a std module that never ran its global
# initialiser is a crash rather than a link error.
BIN="$(find "$TMP/trans/target" -path '*/bin/trans' -type f | head -1)"
[ -n "$BIN" ] || { echo "FAIL: no binary produced by the transitive project"; exit 1; }
"$BIN" || { echo "FAIL: the transitive binary exited $?"; exit 1; }
echo "  transitive project: binary runs"

echo "235 std.o linked only when needed OK"
