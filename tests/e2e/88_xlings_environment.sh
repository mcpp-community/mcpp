#!/usr/bin/env bash
# 88_xlings_environment.sh — L-1 build environment: a project's `[xlings]` section
# is materialized verbatim into <proj>/.mcpp/.xlings.json (the keys xlings already
# reads: deps / workspace / subos / envs), so a project can declare its host
# build-tools, per-tool env vars, pinned tool versions, and a named sandbox.
# See .agents/docs/2026-06-29-manifest-environment-and-platform-design.md (L-1).
#
# requires: gcc
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[xlings]
# Host build-tools the project wants available (xlings "deps").
#
# ⚠️ THE VERSION HAS TO EXIST, AND UNTIL 2026.9.1.1 IT DID NOT HAVE TO.
# This read `make@4.4, cmake@3.28`; the index carries make 4.3 and cmake
# 4.4.2/4.0.2, so neither has ever resolved. That was invisible because #531's
# provisioning did not read its own result — every failure xlings reported was
# taken as success — so this test asserted materialization on top of an install
# that never happened, which is the state #531 was written to end. Now the
# failure is reported and the build stops, which is what makes the fixture's
# error visible at last.
#
# `ninja@1.12.1` is deliberate: it is already installed anywhere mcpp can
# build, so provisioning short-circuits and this test still costs no download.
deps = ["ninja@1.12.1"]
# A named per-project sandbox.
subos = "dev"

[xlings.workspace]
# Pin a tool version (the general form of [toolchain]).
ninja = "1.12.1"

[xlings.envs]
# Env vars applied by xvm shims.
APP_BUILD_ENV = "1"
EOF
echo 'int main() { return 0; }' > app/src/main.cpp

# A named SubOS is an exact, pre-existing local development environment. It
# may not silently fall back to mcpp default/active, so seed this fixture from
# the already bootstrapped default contract before the first build.
mkdir -p app/.mcpp/.xlings/subos/dev
cp "$MCPP_HOME/registry/subos/default/.xlings.json" \
   app/.mcpp/.xlings/subos/dev/.xlings.json

cd app
"$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: build errored"; exit 1; }

# The project .xlings.json must carry the [xlings] section materialized 1:1.
J=.mcpp/.xlings.json
[ -f "$J" ] || { echo "FAIL: $J was not written"; ls -la .mcpp 2>/dev/null; exit 1; }
echo "--- $J ---"; cat "$J"
grep -q '"deps"'           "$J" || { echo "FAIL: deps not materialized"; exit 1; }
grep -q 'ninja@1.12.1'     "$J" || { echo "FAIL: deps entry missing"; exit 1; }
grep -q '"subos": "dev"'   "$J" || { echo "FAIL: subos not materialized"; exit 1; }
grep -q '"workspace"'      "$J" || { echo "FAIL: workspace not materialized"; exit 1; }
grep -q '"ninja": "1.12.1"' "$J" || { echo "FAIL: workspace pin missing"; exit 1; }
grep -q '"envs"'           "$J" || { echo "FAIL: envs not materialized"; exit 1; }
grep -q '"APP_BUILD_ENV": "1"' "$J" || { echo "FAIL: env var missing"; exit 1; }

echo "OK"
