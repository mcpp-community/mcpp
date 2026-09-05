#!/usr/bin/env bash
# requires: gcc
# 611_variant_switch_is_not_replayed.sh — a plain `mcpp build` after
# `mcpp build --no-accel` builds the device variant again (mcpp 2026.9.5.3+).
#
# The device variant is part of the fingerprint, so the two builds land in
# different directories. The fast path, which runs before any plan exists,
# replayed whichever directory was built LAST: after `--no-accel`, a plain
# build reported "Finished in 0.00s" and `mcpp run` executed the CPU variant,
# while the manifest said the project is a device build. The graph now records
# whether an override chose it, and the fast paths decline such a graph.
#
# The fixture is the one e2e 605 uses: a cfg section keyed on `accelerator`
# supplies a define, so the running program reports which variant it is.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new probe > /dev/null; cd probe
rm -f src/*.cppm
cat > mcpp.toml <<'EOT'
[package]
name = "probe"
version = "0.1.0"
[language]
standard = "c++23"

[build]
accel = "widget9+{w1}"

[target.'cfg(accelerator = "widget")'.build]
defines = ["WIDGET_ON=1"]

[targets.probe]
kind = "bin"
main = "src/main.cpp"
EOT
cat > src/main.cpp <<'EOT'
#include <cstdio>
#ifndef WIDGET_ON
#define WIDGET_ON 0
#endif
int main() { std::printf("WIDGET=%d\n", WIDGET_ON); }
EOT

variant() { "$MCPP" run "$@" 2>&1 | grep '^WIDGET=' | tail -1; }

"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: the device build failed"; exit 1; }
[[ "$(variant)" == "WIDGET=1" ]] || { echo "FAIL: the first build is not the device variant"; exit 1; }

"$MCPP" build --no-accel > b2.log 2>&1 || { cat b2.log; echo "FAIL: --no-accel failed"; exit 1; }
[[ "$(variant --no-accel)" == "WIDGET=0" ]] || { echo "FAIL: --no-accel did not produce the CPU variant"; exit 1; }

# The measurement. Before the fix this printed "Finished dev in 0.00s" and the
# run below answered WIDGET=0.
"$MCPP" build > b3.log 2>&1 || { cat b3.log; echo "FAIL: the plain build after --no-accel failed"; exit 1; }
out="$(variant)"
[[ "$out" == "WIDGET=1" ]] || {
    echo "FAIL: a plain build after --no-accel handed back the CPU variant: $out"
    cat b3.log; exit 1; }
echo "PASS: a plain build after --no-accel builds and runs the device variant"

# And the other direction still holds (it did before: the flag bypasses the
# fast path), so the two are symmetric.
"$MCPP" build --no-accel > b4.log 2>&1
[[ "$(variant --no-accel)" == "WIDGET=0" ]] || { echo "FAIL: --no-accel after a plain build is not the CPU variant"; exit 1; }
echo "PASS: --no-accel after a plain build still selects the CPU variant"

# The graph says which selection wrote it, in the file itself.
n_default=$(grep -l '^# mcpp:graph=normal;schedule=[a-z-]*;accel=default' target/*/*/build.ninja | wc -l)
n_override=$(grep -l '^# mcpp:graph=normal;schedule=[a-z-]*;accel=override' target/*/*/build.ninja | wc -l)
[[ "$n_default" -eq 1 && "$n_override" -eq 1 ]] || {
    echo "FAIL: expected one default and one override graph, got default=$n_default override=$n_override"
    head -3 target/*/*/build.ninja; exit 1; }
echo "PASS: each graph records the selection that wrote it"

echo "PASS: a variant switch is not replayed"
