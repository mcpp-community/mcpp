#!/usr/bin/env bash
# requires: gcc
# The probe channel, and the device axis as a build program sees it.
#
# A rule package is the thing that knows how to ask a machine what it has, and
# the engine is the thing that must not. So the package MEASURES and the engine
# COMPARES: `mcpp::fact` states what the machine has, `mcpp::floor` what the
# package needs of it, and the build is refused before anything is compiled
# when the floor is unmet -- naming both values; `mcpp why toolchain --format
# json` classifies the outcome under the reason
# `version-floor-unmet`. The root's build program is where a rule package
# speaks from, so the check has to run AFTER it; before this test it ran only
# before, and a floor stated there was never compared.
#
# The same program reads the resolved `accel` (`MCPP_ACCEL`), and the
# `cfg(accelerator = "...")` layer key is fed from the same value -- a key that
# was declared, documented, and never written before this.
#
# Nothing here names a vendor: `widget` is the backend, and the engine treats
# it exactly as it would any other. That is the property.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new probe > /dev/null; cd probe
rm -f src/*.cppm
cat > src/main.cpp <<'EOF'
// Compiles only when the accelerator layer selected the widget backend: the
// define arrives through a cfg section keyed on `accelerator`.
#ifndef WIDGET_ON
#error "cfg(accelerator = \"widget\") did not match, so WIDGET_ON is missing"
#endif
int main() { return 0; }
EOF

write_manifest() {   # $1 = floor spec
    cat > mcpp.toml <<EOF
[package]
name = "probe"
version = "0.1.0"
[language]
standard = "c++23"

[build]
accel = "widget9+{w1,w2}"

[target.'cfg(accelerator = "widget")'.build]
defines = ["WIDGET_ON"]

[targets.probe]
kind = "bin"
main = "src/main.cpp"
EOF
    cat > build.mcpp <<EOF
import mcpp;
int main() {
    mcpp::warning(mcpp::accel());          // what the program was told
    mcpp::fact("widget.driver", "1.2");    // what the "machine" has
    mcpp::floor("$1");                     // what this package needs
    return 0;
}
EOF
}

# ── One: the floor is above the fact, stated by the ROOT's build program ──
write_manifest "widget.driver >= 2.0"
if "$MCPP" build > refused.log 2>&1; then
    cat refused.log; echo "FAIL: a floor above the stated fact was accepted"; exit 1
fi
grep -q "widget.driver" refused.log || { cat refused.log; echo "FAIL: refusal does not name the quantity"; exit 1; }
grep -q "2.0" refused.log || { cat refused.log; echo "FAIL: refusal does not say what was needed"; exit 1; }
grep -q "1.2" refused.log || { cat refused.log; echo "FAIL: refusal does not say what is there"; exit 1; }
echo "PASS: a floor stated by the root's build program is compared, and refused with both values"

# The machine interface names the reason, so a tool can act on it without
# parsing prose. `why toolchain` runs the same prepare, build program included.
"$MCPP" why toolchain --format json > refused.json 2>/dev/null || true
reason="$(jq -r '.data.reason // "-"' refused.json | tr -d '\r')"
[[ "$reason" == "version-floor-unmet" ]] || {
    cat refused.json; echo "FAIL: reason is '$reason', expected version-floor-unmet"; exit 1; }
echo "PASS: the reason is version-floor-unmet under why toolchain --format json"

# ── Two: the control -- a met floor builds ──────────────────────────────
write_manifest "widget.driver >= 1.0"
touch src/main.cpp
"$MCPP" build > ok.log 2>&1 || { cat ok.log; echo "FAIL: a met floor was refused"; exit 1; }
echo "PASS: a met floor builds"

# ── Three: the build program saw the resolved accel ───────────────────────
#
# The advisory carries whatever `mcpp::accel()` returned. Parsed and printed
# back by the engine, so the spelling is canonical whatever the manifest wrote.
grep -q "widget9+{w1,w2}" ok.log || {
    cat ok.log; echo "FAIL: MCPP_ACCEL did not carry the resolved accel"; exit 1; }
echo "PASS: MCPP_ACCEL carries the resolved accel"

# ── Four: --no-accel empties both the variable and the layer ──────────────
#
# The layer is the sharper half: with no backend enabled the cfg section must
# NOT apply, and then main.cpp's #error fires. A key that still matched here
# would be reading the manifest rather than the build.
if "$MCPP" build --no-accel > none.log 2>&1; then
    cat none.log; echo "FAIL: cfg(accelerator) matched under --no-accel"; exit 1
fi
grep -q "WIDGET_ON is missing" none.log || {
    cat none.log; echo "FAIL: the build failed for another reason than the layer"; exit 1; }
echo "PASS: --no-accel leaves the accelerator layer empty"

echo "PASS: probe channel and accel reach the build program"
