#!/usr/bin/env bash
# 91_target_bare_alias.sh — bare OS-alias sugar for [target.*] conditional tables:
# `[target.linux.build]` ≡ `[target.'cfg(linux)'.build]`. The bare aliases
# windows/linux/macos/unix are never valid triples, so they unambiguously mean the
# cfg predicate. HOST-AWARE: asserts (a) exactly one OS alias applies (the host's)
# and (b) the bare alias agrees with the cfg() form per-OS — so it validates the
# parity on whichever of linux/macos/windows the runner is.
# See .agents/docs/2026-06-30-target-bare-alias-sugar-design.md.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

# Bare-alias forms (the sugar under test).
[target.linux.build]
cxxflags = ["-DBARE_LINUX=1"]
[target.macos.build]
cxxflags = ["-DBARE_MACOS=1"]
[target.windows.build]
cxxflags = ["-DBARE_WIN=1"]
[target.unix.build]
cxxflags = ["-DBARE_UNIX=1"]

# cfg(...) forms — must produce identical results (parity).
[target.'cfg(linux)'.build]
cxxflags = ["-DCFG_LINUX=1"]
[target.'cfg(macos)'.build]
cxxflags = ["-DCFG_MACOS=1"]
[target.'cfg(windows)'.build]
cxxflags = ["-DCFG_WIN=1"]
[target.'cfg(unix)'.build]
cxxflags = ["-DCFG_UNIX=1"]
EOF
cat > app/src/main.cpp <<'EOF'
// Exactly one OS bare-alias must apply — the host's — on any platform.
#if (defined(BARE_LINUX) + defined(BARE_MACOS) + defined(BARE_WIN)) != 1
#error "exactly one bare OS alias (linux/macos/windows) must apply on any host"
#endif
// Bare alias and cfg() must agree per OS (parity).
#if defined(BARE_LINUX) != defined(CFG_LINUX)
#error "[target.linux] disagrees with [target.'cfg(linux)']"
#endif
#if defined(BARE_MACOS) != defined(CFG_MACOS)
#error "[target.macos] disagrees with [target.'cfg(macos)']"
#endif
#if defined(BARE_WIN) != defined(CFG_WIN)
#error "[target.windows] disagrees with [target.'cfg(windows)']"
#endif
#if defined(BARE_UNIX) != defined(CFG_UNIX)
#error "[target.unix] disagrees with [target.'cfg(unix)']"
#endif
// unix family applies iff not windows.
#if defined(BARE_WIN) && defined(BARE_UNIX)
#error "unix wrongly applied on a windows host"
#endif
#if !defined(BARE_WIN) && !defined(BARE_UNIX)
#error "unix should apply on a non-windows host"
#endif
int main() { return 0; }
EOF

cd app
"$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: build errored (bare alias not honored?)"; exit 1; }

echo "OK"
