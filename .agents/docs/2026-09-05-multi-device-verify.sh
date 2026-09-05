#!/usr/bin/env bash
# Ecosystem verification for mcpp 2026.9.5.2, run inside a fresh xlings subos.
#
# ⚠️ EVERY ASSERTION CARRIES ITS OWN `|| fail`. A previous run of this kind was
# reported green because the transport dropped `set -euo pipefail` from the
# first line and the closing banner printed unconditionally. A script whose
# "pass" means "nothing failed" degrades, when that line is gone, into one whose
# "pass" means "it reached the last line", and the two read identically.
set -uo pipefail

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
# The store path is the default and the point (see A); MCPP_VERIFY_BIN exists
# so this script can be rehearsed against a working-tree build before a release
# exists, and is never what a release is verified with.
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"

fails=0
fail()  { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()    { printf 'ok: %s\n' "$1"; }
have()  { command -v "$1" >/dev/null 2>&1; }

section() { printf '\n== %s ==\n' "$1"; }

# ── A. the released binary is the one under test ────────────────────────────
#
# ⭐ The store path, never the shim. `xlings install` has been observed to prune
# bare-name shims that mcpp itself installed, so a shim on PATH may resolve to
# an older version and the whole run would measure the wrong binary.
section "A. identity"
[ -x "$STORE" ] || fail "no released binary at $STORE"
got=$("$STORE" --version 2>&1 | head -1)
[ "$got" = "mcpp $VER" ] || fail "version is '$got', not 'mcpp $VER'"
[ "$got" = "mcpp $VER" ] && ok "$got from the store path"

# ── B. the accelerator surface exists on all three verbs ────────────────────
section "B. the device axis reaches run and test"
for verb in build run test; do
    "$STORE" "$verb" --help 2>&1 | grep -q -- '--no-accel' \
        || fail "\`mcpp $verb --help\` does not mention --no-accel"
done
[ "$fails" -eq 0 ] && ok "build, run and test all take --accel/--no-accel"

# ── C. a project that states a floor nothing satisfies is refused ───────────
#
# The point is that the refusal happens BEFORE a compile, and names both values.
section "C. version floors are compared before compiling"
proj=$(mktemp -d)
mkdir -p "$proj/src"
cat > "$proj/mcpp.toml" <<'EOF'
[package]
name = "floor-probe"
version = "0.1.0"

[language]
standard = "c++23"
modules = true
import_std = true

[build]
sources = ["src/*.cpp"]

[targets.floor-probe]
kind = "bin"
main = "src/main.cpp"
EOF
cat > "$proj/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF
cat > "$proj/build.mcpp" <<'EOF'
import std;
import mcpp;
int main() {
    mcpp::fact("probe.quantity", "1.0");
    mcpp::floor("probe.quantity >= 9.0");
    return 0;
}
EOF
out=$(cd "$proj" && "$STORE" build 2>&1)
printf '%s\n' "$out" | grep -q "requires probe.quantity >= 9.0" \
    || fail "an unmet floor did not refuse the build (output: $(printf '%s' "$out" | tail -3 | tr '\n' ' '))"
printf '%s\n' "$out" | grep -q "and this machine has 1.0" \
    || fail "the refusal did not state the measured value"
printf '%s\n' "$out" | grep -qi "compiling floor-probe" \
    && fail "the build compiled before the floor was compared"
printf '%s\n' "$out" | grep -q "requires probe.quantity >= 9.0" && ok "an unmet floor refuses before compiling, naming both values"

# ── C'. the control: a floor that IS met does not refuse ────────────────────
sed -i 's/probe.quantity >= 9.0/probe.quantity >= 0.5/' "$proj/build.mcpp"
out=$(cd "$proj" && "$STORE" build 2>&1)
printf '%s\n' "$out" | grep -q "Finished" \
    || fail "a satisfied floor still refused the build (output: $(printf '%s' "$out" | tail -3 | tr '\n' ' '))"
printf '%s\n' "$out" | grep -q "Finished" && ok "a satisfied floor builds — the check measures the floor, not the machine"

# ── D. a constrained glob narrows, and --no-accel excludes it ───────────────
#
# ⚠️ THE BUILD PROGRAM WRITES A FILE RATHER THAN PRINTING. mcpp shows a build
# program's stdout only when it exits non-zero, so a probe that printed its
# answer would be invisible on every successful build — and a grep that never
# matches is indistinguishable from one that matches nothing.
section "D. constrained source globs"
proj2=$(mktemp -d)
mkdir -p "$proj2/src/kernels"
cat > "$proj2/mcpp.toml" <<'EOF'
[package]
name         = "glob-probe"
version      = "0.1.0"
accelerators = ["cuda"]

[language]
standard = "c++23"
modules = true
import_std = true

[build]
accel   = "cuda12.9+{sm_89}"
sources = [
  "src/*.cpp",
  { glob = "src/kernels/**/*.cu", accel = "cuda12.9+{sm_89}" },
]

[targets.glob-probe]
kind = "bin"
main = "src/main.cpp"
EOF
cat > "$proj2/src/main.cpp" <<'EOF'
extern "C" int answer();
int main() { return answer() == 7 ? 0 : 1; }
EOF
cat > "$proj2/src/answer.cpp" <<'EOF'
extern "C" int answer() { return 7; }
EOF
cat > "$proj2/src/kernels/k.cu" <<'EOF'
// Never compiled here: no rule package is imported, so a build that hands this
// file to the engine's compile rules is a build that narrowed wrongly.
#error "the device glob must not reach the engine's compile rules"
EOF
cat > "$proj2/build.mcpp" <<'EOF'
import std;
import mcpp;
int main() {
    mcpp::rerun_if_env_changed("MCPP_PROBE_OUT");
    const char* out = std::getenv("MCPP_PROBE_OUT");
    if (out && *out) {
        std::ofstream f(out, std::ios::trunc);
        f << "DEVICE_SOURCES=[" << mcpp::device_sources() << "]\n";
        f << "ACCEL=[" << mcpp::accel() << "]\n";
    }
    return 0;
}
EOF
probe_out="$proj2/probe.txt"
out=$(cd "$proj2" && MCPP_PROBE_OUT="$probe_out" "$STORE" build 2>&1)
printf '%s\n' "$out" | grep -q "Finished" \
    || fail "the device build failed (output: $(printf '%s' "$out" | tail -4 | tr '\n' ' '))"
grep -q "DEVICE_SOURCES=\[src/kernels/k.cu\]" "$probe_out" 2>/dev/null \
    || fail "the device source did not reach the build program (probe: $(cat "$probe_out" 2>/dev/null | tr '\n' ' '))"
grep -q "DEVICE_SOURCES=\[src/kernels/k.cu\]" "$probe_out" 2>/dev/null \
    && ok "a constrained glob reaches the build program as a device source"

rm -f "$probe_out"
out=$(cd "$proj2" && MCPP_PROBE_OUT="$probe_out" "$STORE" build --no-accel 2>&1)
printf '%s\n' "$out" | grep -q "Finished" \
    || fail "--no-accel did not build (output: $(printf '%s' "$out" | tail -4 | tr '\n' ' '))"
grep -q "DEVICE_SOURCES=\[\]" "$probe_out" 2>/dev/null \
    || fail "--no-accel did not empty the device source list (probe: $(cat "$probe_out" 2>/dev/null | tr '\n' ' '))"
grep -q "ACCEL=\[\]" "$probe_out" 2>/dev/null \
    || fail "--no-accel did not empty the accel axis"
grep -q "DEVICE_SOURCES=\[\]" "$probe_out" 2>/dev/null \
    && ok "--no-accel excludes the constrained glob and still builds"

out=$(cd "$proj2" && "$STORE" build --accel 'cuda12.9+{sm_80}' 2>&1)
printf '%s\n' "$out" | grep -q "does not cover" \
    || fail "an accel that does not cover the glob was not refused"
printf '%s\n' "$out" | grep -q "does not cover" && ok "an accel outside the constraint is refused naming both"

# ── E. the engine carries no vendor tool or header name ────────────────────
#
# ⚠️ ASSERTED ON THE BINARY, NOT ON `self doctor`'s SILENCE. An earlier
# revision asked whether `self doctor` mentions a toolkit, and that passes
# vacuously: the section it was watching for only ever printed when a CUDA
# toolkit was installed, and a fresh sandbox has none. Rehearsed against
# 2026.9.5.1 — which still had the reader — the check reported "ok".
#
# The strings are the discriminating ones: `crt/host_config.h` is the header
# the doctor used to read, and it appears twice in the 2026.9.5.1 binary and
# not at all once the reader moved into the rule package. The source-level
# property (no vendor name beside a probe launch anywhere in `src/`) is a unit
# test, `test_core_vendor_probes`, and belongs to CI rather than here.
section "E. the engine carries no vendor tool or header name"
vendor_hits=0
for word in host_config cicc cudafe fatbinary nvidia-smi; do
    # ⚠️ NO `|| echo 0`. `grep -c` PRINTS 0 and EXITS 1 when it matches nothing,
    # so the fallback appended a second line and `[` saw "0\n0" — "integer
    # expression expected", the assertion skipped, and the section still
    # reported ok. Measured while writing this.
    n=$(grep -a -c -- "$word" "$STORE" 2>/dev/null); rc=$?
    [ "$rc" -gt 1 ] && { fail "could not read $STORE while looking for '$word'"; continue; }
    [ "${n:-0}" -gt 0 ] && { fail "the engine binary contains '$word' ($n)"; vendor_hits=1; }
done
[ "$vendor_hits" -eq 0 ] && ok "no vendor tool or header name is in the engine binary"

# ── verdict ────────────────────────────────────────────────────────────────
printf '\n'
if [ "$fails" -eq 0 ]; then
    printf 'ALL ASSERTIONS PASSED for mcpp %s\n' "$VER"
    exit 0
fi
printf '%d ASSERTION(S) FAILED for mcpp %s\n' "$fails" "$VER"
exit 1
