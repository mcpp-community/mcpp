#!/usr/bin/env bash
# 85_target_cfg_build_flags.sh — L1 platform-conditional config: a normal mcpp.toml
# can scope [build] flags to a target predicate via `[target.'cfg(...)'.build]`.
# The predicate is evaluated against the RESOLVED TARGET (here: the host build's
# own triple), NOT textually — so `cfg(linux)`/`cfg(unix)` flags apply on a Linux
# runner and `cfg(windows)` flags do NOT. See
# .agents/docs/2026-06-29-manifest-environment-and-platform-design.md (L1).
#
# requires: linux
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

# Matching predicate (Linux host) → these cxxflags apply.
[target.'cfg(linux)'.build]
cxxflags = ["-DCOND_LINUX=1"]

# Also matches on Linux (unix family alias).
[target.'cfg(unix)'.build]
cxxflags = ["-DCOND_UNIX=1"]

# Non-matching predicate → must NOT apply on a Linux build.
[target.'cfg(windows)'.build]
cxxflags = ["-DCOND_WIN=1"]

# Boolean combinator: linux AND NOT aarch64 (x86_64 runner) → applies.
[target.'cfg(all(linux, not(arch = "aarch64")))'.build]
cxxflags = ["-DCOND_X64_LINUX=1"]
EOF
cat > app/src/main.cpp <<'EOF'
// The conditional cxxflags must reach this TU. Missing/!expected → #error.
#ifndef COND_LINUX
#error "cfg(linux) cxxflag did not apply on a Linux build"
#endif
#ifndef COND_UNIX
#error "cfg(unix) cxxflag did not apply on a Linux build"
#endif
#ifdef COND_WIN
#error "cfg(windows) cxxflag wrongly applied on a Linux build"
#endif
#ifndef COND_X64_LINUX
#error "cfg(all(linux, not(arch=aarch64))) cxxflag did not apply on x86_64 Linux"
#endif
int main() { return 0; }
EOF

cd app
"$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: build errored (conditional flag mis-applied?)"; exit 1; }

# The matching flag must be on the consumer TU; the non-matching one must not.
grep -q 'COND_LINUX=1'   compile_commands.json || { echo "FAIL: cfg(linux) flag absent from compile db"; exit 1; }
grep -q 'COND_X64_LINUX=1' compile_commands.json || { echo "FAIL: cfg(all/not) flag absent"; exit 1; }
if grep -q 'COND_WIN=1' compile_commands.json; then
    echo "FAIL: cfg(windows) flag leaked into a Linux build's compile db"; exit 1; fi

echo "OK"
