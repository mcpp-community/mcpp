#!/usr/bin/env bash
# requires: gcc unix-shell
# 900 -- `mcpp publish` writes what the workspace layer gave a member into the
# member's own tables (W6, SPEC-004 §9.7 and §9.10; design 2026-10-10 §3.2).
#
# The archive holds the member's directory alone. Built as the root of a
# host-tool sub-build from an index, it must be configured as it was in its
# workspace, so `[workspace.toolchain]` is written as `[toolchain]` and
# `[workspace.target.<sel>.build]` as `[target.<sel>.build]`, merged as the
# build merged them.
#
#   W1  the archived mcpp.toml carries `[toolchain]` and the conditional row;
#   W2  a relative path the workspace wrote is rewritten against the package;
#   W3  one that leaves the package is refused, naming the key.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

REPO="$TMP/repo"
mkdir -p "$REPO/util/src" "$REPO/util/inc"
cd "$REPO"
cat > mcpp.toml <<'TOML'
[workspace]
members = ["util"]

[workspace.package]
version = "0.3.0"
license = "MIT"
repo    = "https://github.com/example/probe"

[workspace.toolchain]
default = "gcc@16.1.0"

[workspace.target.'cfg(os = "linux")'.build]
cxxflags = ["-DWS_COND=1"]
include_dirs = ["util/inc"]
TOML
cat > util/mcpp.toml <<'TOML'
[package]
namespace = "probe"
name = "util"

[target.'cfg(os = "linux")'.build]
cxxflags = ["-DOWN_COND=1"]

[targets.probe_util]
kind = "lib"
TOML
cat > util/src/util.cppm <<'CPP'
export module probe.util;
export int util_value() { return 42; }
CPP
git init -q -b main . && git add -A \
    && git -c user.email=e2e@example.invalid -c user.name=e2e commit -qm init

# ── W1, W2 ────────────────────────────────────────────────────────────────
( cd util && "$MCPP" publish --dry-run --allow-dirty > "$TMP/pub.log" 2>&1 ) \
    || fail "publish of a member with a workspace layer failed" "$TMP/pub.log"
ARCHIVE="$REPO/util/target/dist/util-0.3.0.tar.gz"
[ -f "$ARCHIVE" ] || fail "no archive at $ARCHIVE" "$TMP/pub.log"
tar -xzf "$ARCHIVE" -O util-0.3.0/mcpp.toml > "$TMP/util.toml"
python3 - "$TMP/util.toml" <<'PY' || fail "the archived manifest is not what the member was built with" "$TMP/util.toml"
import sys, tomllib
m = tomllib.load(open(sys.argv[1], "rb"))
assert m["toolchain"]["default"] == "gcc@16.1.0", m.get("toolchain")
row = m["target"]['cfg(os = "linux")']["build"]
assert row["cxxflags"] == ["-DWS_COND=1", "-DOWN_COND=1"], row
assert row["include_dirs"] == ["inc"], row
PY
echo "ok W1 W2"

# ── W3 ────────────────────────────────────────────────────────────────────
sed -i.bak 's|include_dirs = \["util/inc"\]|include_dirs = ["shared/inc"]|' mcpp.toml && rm -f mcpp.toml.bak
mkdir -p shared/inc
if ( cd util && "$MCPP" publish --dry-run --allow-dirty > "$TMP/pub-out.log" 2>&1 ); then
    fail "a workspace path outside the package was published" "$TMP/pub-out.log"
fi
grep -q "outside this package's directory" "$TMP/pub-out.log" \
    || fail "the refusal does not say why" "$TMP/pub-out.log"
echo "ok W3"

echo "PASS: 900_publish_writes_the_workspace_layer_into_the_member"
