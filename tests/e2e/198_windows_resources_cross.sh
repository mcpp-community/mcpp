#!/usr/bin/env bash
# requires: mingw-cross
# mcpp#365 — Linux → Windows through the MinGW cross toolchain. The GNU dialect
# takes the other fork: `windres -O coff`, because GNU ld cannot consume a .res
# at all. Same assertions as 197 (see _windows_resources_body.sh), plus the
# non-PE half that only a cross host can check: the very same manifest must
# build for the host with the section simply inapplicable.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="${MCPP_HOME:-$HOME/.mcpp}"

BUILD_ARGS="--target x86_64-windows-gnu"
EXE_SUFFIX=".exe"
source "$(dirname "$0")/_windows_resources_body.sh"

# ── A3. On a non-PE target the section is INAPPLICABLE ────────────────────
#
# Not "degraded", not "skipped with a warning": there is no consumer, so the
# build is unchanged and says nothing. This is what makes cfg(windows) gating
# unnecessary — and it is also the half of issue #365's third request that IS
# satisfied (a Windows-only declaration must not break other platforms).
cd "$TMP/proj"
cat > mcpp.toml <<'TOML'
[package]
name    = "resapp"
version = "1.2.3"

[resources]
icon = "assets/app.ico"

[targets.resapp]
kind = "bin"
main = "src/main.cpp"
TOML
"$MCPP" build > host.log 2>&1 || { cat host.log; echo "FAIL: host build with [resources] failed"; exit 1; }
grep -qi 'resource' host.log && { cat host.log; echo "FAIL: a non-PE build must say nothing about resources"; exit 1; }
HOST_DIR=$(dirname "$(find target -name 'build.ninja' -print | xargs grep -L 'rc_object' | head -1)")
[ -d "$HOST_DIR/res" ] && { echo "FAIL: a non-PE build must not emit resource units"; exit 1; }

# ── A4. "inapplicable" stops at COMPILATION, not at validation ────────────
#
# Whether a declared path exists is a fact about the working tree, not about the
# target. Gating the existence check on is_pe() meant a Linux or macOS CI could
# not see a typo in `icon = ...` at all and only the Windows job went red — the
# same "find out late" failure the hard error exists to remove. So: nothing is
# compiled here, nothing is said when the files are fine, and a missing one is
# still an error.
sed -i.bak 's|^icon = .*|icon = "assets/typo.ico"|' mcpp.toml && rm -f mcpp.toml.bak
"$MCPP" build > host2.log 2>&1 \
    && { cat host2.log; echo "FAIL: a missing declared resource must fail on non-PE targets too"; exit 1; }
grep -q 'does not exist' host2.log \
    || { cat host2.log; echo "FAIL: expected the same missing-file error as on Windows"; exit 1; }

echo "OK"
