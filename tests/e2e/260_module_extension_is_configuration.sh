#!/usr/bin/env bash
# requires:
# (no capability: mcpp states a module interface's LANGUAGE explicitly per
#  toolchain, so a declared extension behaves the same on all three.)
#
# 260_module_extension_is_configuration.sh — `[build] module_extensions` is the
# knob, and everything downstream follows it without a second declaration.
#
# `.ixx` is NOT built in, on purpose: the extension set is configuration, not a
# list mcpp grows one entry at a time as extensions come into fashion. What that
# demands in return is two things, and this test is both of them:
#
#   1. a DECLARED extension works with no further help;
#   2. an UNDECLARED one is refused where the mistake is, not four steps later.
#
# ⚠️ WHY (2) IS THE INTERESTING HALF. Before this, an undeclared `.ixx` matched
# by `sources` was accepted and produced a compile edge whose object NOTHING
# LINKS — measured:
#
#   build obj/mathkit.ixx.o | gcm.cache/mathkit.gcm : cxx_object …
#     bmi_out = gcm.cache/mathkit.gcm         ← the BMI was produced
#   build bin/app : cxx_link obj/main.o       ← the object is not here
#
#   ld: undefined reference to `mk::answer@mathkit()'
#
# Two answers to "is this a module interface" and only one of them read: the
# scanner sees `export module` and records `provides`, which is why the edge got
# a `bmi_out`; the classifier says `Other`, and the link set is built from the
# classifier. So the author was told about a symbol when what happened was a
# missing line of configuration.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p proj/src
cat > proj/src/mathkit.ixx <<'EOF'
export module mathkit;
export namespace mk { int answer() { return 42; } }
EOF
cat > proj/src/main.cpp <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("ixx-ok=%d\n", mk::answer()); return 0; }
EOF

manifest() {   # $1 = extra [build] body
    cat > "$TMP/proj/mcpp.toml" <<EOF
[package]
name    = "ixxprobe"
version = "0.1.0"
[build]
sources = ["src/*.ixx", "src/*.cpp"]
$1
[targets.ixxprobe]
kind = "bin"
main = "src/main.cpp"
EOF
}

cd proj

# ── 1. undeclared: refused, and the message names the key ───────────────
manifest ''
rm -rf target
if "$MCPP" build > undeclared.log 2>&1; then
    echo "FAIL: an undeclared '.ixx' was accepted."
    nj="$(find target -name build.ninja | head -1)"
    grep -n 'mathkit' "$nj" | head
    echo "      Check whether its object is in the link edge — if it is not,"
    echo "      this 'success' produces an undefined reference at link time."
    exit 1
fi
grep -q "no role for the extension '.ixx'" undeclared.log || {
    cat undeclared.log
    echo "FAIL: refused, but not for the classification reason. A message that"
    echo "      does not name the extension leaves the author with a symbol."
    exit 1; }
grep -q "module_extensions" undeclared.log || {
    cat undeclared.log
    echo "FAIL: the message does not name the key that fixes it, so it is a"
    echo "      diagnosis without a remedy."
    exit 1; }

# ── 2. declared: works, on this toolchain, with nothing else ────────────
manifest 'module_extensions = [".ixx"]'
rm -rf target
"$MCPP" run > declared.log 2>&1 || { cat declared.log; echo "FAIL: declared .ixx did not build"; exit 1; }
grep -q 'ixx-ok=42' declared.log || { cat declared.log; echo "FAIL: wrong answer"; exit 1; }

echo "PASS: a declared module extension needs no further help; an undeclared one is refused"
