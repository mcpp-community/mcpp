#!/usr/bin/env bash
# 85_target_cfg_build_flags.sh — L1 platform-conditional config: a normal mcpp.toml
# scopes [build] flags to a target predicate via `[target.'cfg(...)'.build]`. The
# predicate is evaluated against the RESOLVED target (here the host, a native
# build) — NOT textually — so exactly the host's os/arch predicates apply. This
# test is HOST-AWARE: it asserts the correct subset applies on whichever of
# linux/macos/windows + x86_64/aarch64 the runner is, so it validates the
# evaluator on all three CI platforms. See
# .agents/docs/2026-06-29-manifest-environment-and-platform-design.md (L1).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[target.'cfg(linux)'.build]
cxxflags = ["-DCOND_LINUX=1"]
[target.'cfg(macos)'.build]
cxxflags = ["-DCOND_MACOS=1"]
[target.'cfg(windows)'.build]
cxxflags = ["-DCOND_WIN=1"]
[target.'cfg(unix)'.build]
cxxflags = ["-DCOND_UNIX=1"]
[target.'cfg(arch = "x86_64")'.build]
cxxflags = ["-DCOND_X64=1"]
[target.'cfg(arch = "aarch64")'.build]
cxxflags = ["-DCOND_ARM64=1"]
# Boolean combinator: any(linux, macos) == unix-family.
[target.'cfg(any(linux, macos))'.build]
cxxflags = ["-DCOND_UNIXLIKE=1"]
EOF
cat > app/src/main.cpp <<'EOF'
// Exactly one OS predicate must apply (the host's), regardless of platform.
#if (defined(COND_LINUX) + defined(COND_MACOS) + defined(COND_WIN)) != 1
#error "exactly one of cfg(linux)/cfg(macos)/cfg(windows) must apply on any host"
#endif
// cfg(unix) applies iff not windows.
#if defined(COND_WIN) && defined(COND_UNIX)
#error "cfg(unix) wrongly applied on a windows host"
#endif
#if !defined(COND_WIN) && !defined(COND_UNIX)
#error "cfg(unix) should apply on a non-windows host"
#endif
// any(linux, macos) must agree with unix on the CI platforms.
#if defined(COND_UNIXLIKE) != defined(COND_UNIX)
#error "cfg(any(linux,macos)) disagreed with cfg(unix)"
#endif
// Exactly one arch predicate must apply on the CI runners (x86_64 or aarch64).
#if (defined(COND_X64) + defined(COND_ARM64)) != 1
#error "exactly one of cfg(arch=x86_64)/cfg(arch=aarch64) must apply"
#endif
// Mutual exclusion: the non-host OS predicate must NOT leak in.
int main() { return 0; }
EOF

cd app
# The #error guards ARE the assertion: a clean build proves the right conditional
# cxxflags reached the TU and the wrong ones did not.
"$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: conditional cfg flags mis-applied for this host"; exit 1; }

echo "OK"
