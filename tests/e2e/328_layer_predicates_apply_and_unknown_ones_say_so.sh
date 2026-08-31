#!/usr/bin/env bash
# 328_layer_predicates_apply_and_unknown_ones_say_so.sh — #540 / #494.
#
# docs/14 documents a package adapting to the C library it was built over:
#
#     [target.'cfg(c-abi = "musl")'.build]
#     std-module-flags = ["-D_GNU_SOURCE"]        # wrong for picolibc
#
# and #494 moved those keys onto BuildInputs FOR this, its member comment
# saying membership "is what makes the cfg axis carry it". Nothing evaluated
# the predicate: `cfgpred::Ctx` was built from the target triple alone, so
# `match_kv` knew only os/arch/family/env and every layer section was dropped —
# in SILENCE, because a predicate that answers false and a predicate that was
# never understood produced the same nothing. Measured on 2026.8.30.2: a
# `cfg(c-abi = "glibc")` section contributed no define and emitted no warning.
#
# Three properties:
#   (1) a layer predicate that matches APPLIES;
#   (2) a layer predicate that does not match does NOT apply — otherwise (1)
#       is satisfied by a pass that fires unconditionally;
#   (3) a key outside the vocabulary is REPORTED, and is an error under
#       --strict.
#
# requires: gcc unix-shell
set -e

MCPP="${MCPP:-mcpp}"
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# The resolved c-abi for this build, read from mcpp's own report rather than
# assumed. ⚠️ The report prints layers whose origin is not the compiler payload
# by default, so the zero-config case needs MCPP_VERBOSE to list all five.
mkdir -p probe/src
cat > probe/mcpp.toml <<'EOF'
[package]
name    = "probe"
version = "0.1.0"
EOF
echo 'int main() { return 0; }' > probe/src/main.cpp
( cd probe && MCPP_VERBOSE=1 "$MCPP" build > report.log 2>&1 ) || {
    cat probe/report.log; echo "FAIL: the probe build errored"; exit 1; }
CABI=$(sed -n 's/.*c-abi  *\([A-Za-z0-9_+-]*\).*/\1/p' probe/report.log | head -1)
[ -n "$CABI" ] || { cat probe/report.log; echo "FAIL: could not read the resolved c-abi"; exit 1; }
echo "  ..  this host resolves c-abi = '$CABI'"

# ⚠️ A NAME NO LAYER CAN TAKE, for the negative leg. Deriving it (rather than
# hardcoding "musl") keeps the leg meaningful on a musl host, where a hardcoded
# counter-example would be the TRUE case and the test would assert nothing.
NOTCABI="not-${CABI}"

# ── (1) + (2) a matching layer predicate applies; a non-matching one does not ──
mkdir -p layers/src
cat > layers/mcpp.toml <<EOF
[package]
name    = "layers"
version = "0.1.0"

[target.'cfg(c-abi = "$CABI")'.build]
defines = ["PROBE_MATCHED=1"]

[target.'cfg(c-abi = "$NOTCABI")'.build]
defines = ["PROBE_MUST_NOT_APPLY=1"]

[target.'cfg(all(unix, c-abi = "$CABI"))'.build]
defines = ["PROBE_COMBINED=1"]
EOF
cat > layers/src/main.cpp <<'EOF'
#ifndef PROBE_MATCHED
#error "a matching cfg(c-abi = ...) section did not apply"
#endif
#ifdef PROBE_MUST_NOT_APPLY
#error "a NON-matching cfg(c-abi = ...) section applied"
#endif
#ifndef PROBE_COMBINED
#error "cfg(all(unix, c-abi = ...)) did not apply"
#endif
int main() { return 0; }
EOF
( cd layers && "$MCPP" build > b.log 2>&1 ) || {
    cat layers/b.log; echo "FAIL: layer predicates did not behave"; exit 1; }
echo "  ok  a matching layer predicate applies, a non-matching one does not"

# ⚠️ EXACTLY ONCE, NOT AT LEAST ONCE. The pass that evaluates layer predicates
# runs AFTER the triple-only merge, and `append()` is additive — so a predicate
# with a triple leg (`any(unix, c-abi = ...)`) would be matched by both passes
# and contribute twice. Counting the flag is the only way to see that; a
# preprocessor check cannot tell one -D from two.
mkdir -p once/src
cat > once/mcpp.toml <<EOF
[package]
name    = "once"
version = "0.1.0"

[target.'cfg(any(unix, c-abi = "$CABI"))'.build]
defines = ["PROBE_ONCE=1"]
EOF
echo 'int main() { return 0; }' > once/src/main.cpp
( cd once && "$MCPP" build > b.log 2>&1 ) || {
    cat once/b.log; echo "FAIL: the mixed-predicate build errored"; exit 1; }
n=$(grep -o '\-DPROBE_ONCE=1' once/compile_commands.json | wc -l | tr -d ' ')
[ "$n" -eq 1 ] || {
    echo "FAIL: a predicate with both a triple leg and a layer leg contributed $n times, expected 1"
    grep -o '\-DPROBE_ONCE=1' once/compile_commands.json
    exit 1; }
echo "  ok  a predicate naming both a triple key and a layer applies exactly once"

# ── (2b) …and it reaches a DEPENDENCY, which is the motivating case ─────────
#
# ⚠️ EVERY LEG ABOVE IS SATISFIED BY A PASS THAT ONLY PATCHES packages[0].
# docs/14 writes this feature for "a package supplying a layer [that] frequently
# supports several implementations of the layer beneath it" — a LIBRARY, reached
# as someone's dependency. The build.mcpp tail that shares this pass's window
# patches the root alone, correctly for its own purpose, so copying that shape
# would have left the one package this feature exists for unserved and every
# root-only assertion still green.
mkdir -p dep/app/src dep/lib/src
cat > dep/lib/mcpp.toml <<EOF
[package]
name    = "layerlib"
version = "0.1.0"

[target.'cfg(c-abi = "$CABI")'.build]
defines = ["LIB_MATCHED=1"]

[target.'cfg(c-abi = "$NOTCABI")'.build]
defines = ["LIB_MUST_NOT_APPLY=1"]
EOF
cat > dep/lib/src/layerlib.cppm <<'EOF'
export module layerlib;
#ifndef LIB_MATCHED
#error "a DEPENDENCY's matching cfg(c-abi = ...) section did not apply"
#endif
#ifdef LIB_MUST_NOT_APPLY
#error "a DEPENDENCY's non-matching cfg(c-abi = ...) section applied"
#endif
export int lib_answer() { return 42; }
EOF
cat > dep/app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
layerlib = { path = "../lib" }
EOF
cat > dep/app/src/main.cpp <<'EOF'
import layerlib;
int main() { return lib_answer() == 42 ? 0 : 1; }
EOF
( cd dep/app && "$MCPP" build > b.log 2>&1 ) || {
    cat dep/app/b.log; echo "FAIL: layer predicates did not reach a dependency"; exit 1; }
echo "  ok  a layer predicate reaches a dependency, and its negative leg holds"

# ── (3) an unknown key is reported, and --strict makes it an error ──────────
mkdir -p unknown/src
cat > unknown/mcpp.toml <<'EOF'
[package]
name    = "unknown"
version = "0.1.0"

[target.'cfg(no-such-key = "x")'.build]
defines = ["NEVER"]
EOF
echo 'int main() { return 0; }' > unknown/src/main.cpp
( cd unknown && "$MCPP" build > w.log 2>&1 ) || {
    cat unknown/w.log; echo "FAIL: an unknown cfg key must warn, not fail"; exit 1; }
grep -q "no-such-key" unknown/w.log || {
    cat unknown/w.log; echo "FAIL: the unknown cfg key was not named"; exit 1; }
# The message must list the vocabulary it checked against — built FROM that
# list, so it cannot drift from the check the way three other key lists did.
grep -q "c-abi" unknown/w.log || {
    cat unknown/w.log; echo "FAIL: the message does not list the supported keys"; exit 1; }
echo "  ok  an unknown cfg key is named, with the vocabulary it was checked against"

set +e
( cd unknown && "$MCPP" build --strict > s.log 2>&1 )
rc=$?
set -e
[ "$rc" -ne 0 ] || { cat unknown/s.log; echo "FAIL: --strict did not turn the warning into an error"; exit 1; }
echo "  ok  --strict makes it an error"

echo "PASS: 328 layer predicates apply, and unknown ones say so"
