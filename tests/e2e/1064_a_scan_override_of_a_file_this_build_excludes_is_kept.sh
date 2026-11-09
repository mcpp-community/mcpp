#!/usr/bin/env bash
# requires: llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1064 -- a scan override of a file this build excludes is kept for the builds that compile it
# (design 2026-11-10 E10; mcpp#902; docs/04 §2.8.3).
#
# A `[scan_overrides."<glob>"]` whose files exist and take no part in this build -- a feature that
# is off, a `!` rule of a `[target.<selector>.build]` -- describes them for the builds that do
# compile them. A glob that matches no file of the package is still a typo.
#
# Criteria: with the feature off and with the file excluded by a target rule, the build succeeds;
# with the feature on, the overridden unit (an `import std` under `#ifdef`, which the scanner
# refuses) builds; a glob naming no file is refused.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/src"
cat > "$t/mcpp.toml" <<'TOML'
[package]
name    = "sorepro"
version = "0.1.0"

[build]
sources = ["src/core.cppm", "src/extra.cppm", "src/only_elsewhere.cppm"]

[features]
default = []
[features.extra]
sources = ["src/extra.cppm"]

[target.'cfg(unix)'.build]
sources = ["!src/only_elsewhere.cppm"]

[scan_overrides."src/extra.cppm"]
provides = ["sorepro.extra"]
imports  = []

[scan_overrides."src/only_elsewhere.cppm"]
provides = ["sorepro.elsewhere"]
imports  = []

[lib]
path = "src/core.cppm"
TOML
echo 'export module sorepro; export int core() { return 1; }' > "$t/src/core.cppm"
cat > "$t/src/extra.cppm" <<'CPP'
module;
#ifdef SOREPRO_USE_STD_MODULE
import std;
#endif
export module sorepro.extra;
export int extra() { return 2; }
CPP
cat > "$t/src/only_elsewhere.cppm" <<'CPP'
module;
#ifdef SOREPRO_USE_STD_MODULE
import std;
#endif
export module sorepro.elsewhere;
export int elsewhere() { return 3; }
CPP
cd "$t"
out=$("$MCPP" build --toolchain "llvm@${LLVM_VERSION}" 2>&1) || fail "the build with the feature off failed" "$out"
echo "  ok  overrides of a file a feature leaves out and of one a target rule removes are kept"
out=$("$MCPP" build --toolchain "llvm@${LLVM_VERSION}" --features extra 2>&1) || fail "the build with the feature on failed" "$out"
echo "  ok  with the feature on, the overridden unit builds"
cat >> mcpp.toml <<'TOML'

[scan_overrides."src/nowhere.cppm"]
provides = ["sorepro.nowhere"]
imports  = []
TOML
if out=$("$MCPP" build --toolchain "llvm@${LLVM_VERSION}" 2>&1); then fail "a glob naming no file was accepted" "$out"; fi
grep -q "scan_overrides glob 'src/nowhere.cppm' matches no file of the package" <<<"$out" \
    || fail "the refusal does not name the glob" "$out"
echo "  ok  a glob that names no file is refused"
echo "PASS: 1064 a scan override of a file this build excludes is kept"
