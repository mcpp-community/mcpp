#!/usr/bin/env bash
# requires:
# 167_build_defines_module_scan.sh — mcpp#296: `[build].defines` is the
# package-level macro channel. It must reach EVERY TU (module interface units
# included) on BOTH the compile edge and the P1689 module scan, and the cfg
# axis must be able to carry it like any other build input.
#
# The original report: an `import` guarded by a `[build].defines` macro was
# invisible to the scanner while `scan_overrides` asserted it existed, so the
# build died with "module-graph divergence" naming neither the key nor the
# manifest — because the key was read by nothing and dropped in silence.
#
# Deliberately toolchain-neutral: the module-graph half uses a LOCAL module
# rather than `import std`, so this runs on every platform — which matters,
# since #296 was reported on Windows. Macro delivery is asserted with #error
# guards, so the compiler itself is the assertion (cf. 85_target_cfg_build_flags).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── 1. the #296 shape: a macro-guarded import must reach the P1689 scan ──
# scan_overrides declares that `mid` imports `lib`. That only holds if
# -DUSE_LIB reached the SCAN of mid.cppm; otherwise the scanner sees no import
# and the planner's assumption diverges from the compiler's.
mkdir -p scan/src
cat > scan/src/lib.cppm <<'EOF'
export module lib;
export int lib_value() { return 42; }
EOF
cat > scan/src/mid.cppm <<'EOF'
export module mid;
#ifdef USE_LIB
import lib;
#else
#error "[build].defines did not reach the module interface unit mid.cppm"
#endif
export int mid_value() { return lib_value(); }
EOF
cat > scan/src/main.cpp <<'EOF'
import mid;
// The same macro must reach an ordinary TU, and must carry a value and a
// value CONTAINING A SPACE (the -D quoting path, where the space is part of
// one argv token rather than a token boundary — mcpp#234).
#ifndef USE_LIB
#error "[build].defines did not reach main.cpp"
#endif
#if LEVEL != 3
#error "[build].defines dropped or mangled a NAME=value entry"
#endif
BIGINT wide = 1;
int main() { return (mid_value() == 42 && wide == 1) ? 0 : 1; }
EOF
cat > scan/mcpp.toml <<'EOF'
[package]
name    = "scan"
version = "0.1.0"

[build]
defines = ["USE_LIB", "LEVEL=3", "BIGINT=long long"]

[scan_overrides."src/lib.cppm"]
provides = ["lib"]

[scan_overrides."src/mid.cppm"]
provides = ["mid"]
imports  = ["lib"]

[scan_overrides."src/main.cpp"]
imports  = ["mid"]
EOF

cd scan
"$MCPP" build > build.log 2>&1 || { cat build.log
    echo "FAIL: [build].defines did not reach the scan and/or the compile"; exit 1; }
# Run on its own line, not inside a pipeline: `x=$(cmd | tail)` takes the exit
# status of `tail`, so a crashing binary would be reported as a string mismatch
# (or not at all).
"$MCPP" run > run.log 2>&1 || { cat run.log; echo "FAIL: built binary did not run"; exit 1; }
cd "$TMP"

# ── 2. the cfg axis carries `defines` too (#296 regression guard) ──
# `defines` is a BuildInputs member, so `[target.'cfg(...)'.build]` must merge
# it. HOST-AWARE, like 85_target_cfg_build_flags.sh: exactly the host's
# predicates apply, on whichever platform the runner is.
mkdir -p cond/src
cat > cond/src/main.cpp <<'EOF'
#if (defined(COND_LINUX) + defined(COND_MACOS) + defined(COND_WIN)) != 1
#error "exactly one conditional [build].defines entry must apply on any host"
#endif
#ifndef BASE_DEFINE
#error "the unconditional [build].defines entry was lost when a cfg section merged"
#endif
int main() { return 0; }
EOF
cat > cond/mcpp.toml <<'EOF'
[package]
name    = "cond"
version = "0.1.0"

[build]
defines = ["BASE_DEFINE"]

[target.'cfg(linux)'.build]
defines = ["COND_LINUX"]
[target.'cfg(macos)'.build]
defines = ["COND_MACOS"]
[target.'cfg(windows)'.build]
defines = ["COND_WIN"]
EOF
cd cond
"$MCPP" build > build.log 2>&1 || { cat build.log
    echo "FAIL: conditional [build].defines did not reach the TU"; exit 1; }
cd "$TMP"

# ── 3. a DEPENDENCY's own [build].defines reaches the dependency's TUs ──
# The root package must not be the only one folded: prepare_build folds at the
# path/git-dep site too, mirroring the #229 conditional-merge funnel.
mkdir -p app/src dep/src
cat > dep/src/dep.cppm <<'EOF'
export module dep;
#ifndef DEP_OWN_DEFINE
#error "the dependency's own [build].defines did not reach its module unit"
#endif
export int dep_value() { return 7; }
EOF
cat > dep/mcpp.toml <<'EOF'
[package]
name    = "dep"
version = "0.1.0"

[build]
defines = ["DEP_OWN_DEFINE"]

[targets.dep]
kind = "lib"
EOF
cat > app/src/main.cpp <<'EOF'
import dep;
// A dependency's defines are PRIVATE build inputs — they must not leak into
// the consumer (propagating macros is what feature `defines` are for).
#ifdef DEP_OWN_DEFINE
#error "a dependency's [build].defines leaked into the consumer"
#endif
int main() { return dep_value() == 7 ? 0 : 1; }
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
dep = { path = "../dep" }
EOF
cd app
"$MCPP" build > build.log 2>&1 || { cat build.log
    echo "FAIL: a dependency's [build].defines did not reach its own TUs"; exit 1; }
cd "$TMP"

# ── 4. an unknown [build] key is reported, not silently dropped ──
# The root cause of #296: `defines` was not a key at all, and [build] swallowed
# it without a word. Same policy as [targets.<name>] — a warning, an error
# under --strict.
mkdir -p unknown/src
cat > unknown/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > unknown/mcpp.toml <<'EOF'
[package]
name    = "unknown"
version = "0.1.0"

[build]
defnes = ["TYPO"]
EOF
cd unknown
"$MCPP" build > build.log 2>&1 || { cat build.log
    echo "FAIL: an unknown [build] key must warn, not fail the build"; exit 1; }
grep -q "defnes" build.log || {
    cat build.log; echo "FAIL: unknown [build] key 'defnes' was dropped in silence"; exit 1; }
if "$MCPP" build --strict > strict.log 2>&1; then
    cat strict.log
    echo "FAIL: --strict must reject an unknown [build] key"; exit 1
fi
grep -q "defnes" strict.log || {
    cat strict.log; echo "FAIL: --strict rejection did not name the offending key"; exit 1; }

echo "OK"
