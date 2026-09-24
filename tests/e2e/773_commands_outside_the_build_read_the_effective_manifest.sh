#!/usr/bin/env bash
# requires: gcc
# 773_commands_outside_the_build_read_the_effective_manifest.sh — #690, F5a/F6.
#
# `prepare_build` applies workspace inheritance to the manifest a command
# names. `toolchain list` and `pack` read the project manifest on their own,
# and before #690 they read the raw file:
#
#   - `toolchain list` inside a member without `[toolchain]` marked the global
#     default, while `mcpp build` in the same directory used the workspace's;
#   - `pack` inside a member that leaves `version` to `[workspace.package]`
#     failed with "missing required field 'package.version'" before any
#     routing question was asked.
#
# Each check has a control at the workspace root or in a member that declares
# the value itself, so a reading that never depended on inheritance cannot
# pass as one that does.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

fail() { echo "FAIL: $*"; exit 1; }

WS="$TMP/ws"
mkdir -p "$WS/app/src" "$WS/pinned/src"
cd "$WS"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["app", "pinned"]

[workspace.package]
version = "0.2.0"

[toolchain]
default = "gcc@16.1.0"
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name = "app"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
echo 'int main() { return 0; }' > app/src/main.cpp
# The control declares its own version as well, so that it holds for an
# implementation without the fix and isolates the one difference under test.
cat > pinned/mcpp.toml <<'EOF'
[package]
name = "pinned"
version = "0.2.0"

[toolchain]
default = "gcc@16.1.0"

[targets.pinned]
kind = "bin"
main = "src/main.cpp"
EOF
echo 'int main() { return 0; }' > pinned/src/main.cpp

# ── toolchain list ────────────────────────────────────────────────────────
LEGEND='effective toolchain from project mcpp.toml'
"$MCPP" toolchain list > "$TMP/root.log" 2>&1 || true
if grep -q 'no toolchains installed' "$TMP/root.log"; then
    echo "SKIP toolchain list: no toolchain payload is visible in this home"
else
    grep -q "$LEGEND" "$TMP/root.log" \
        || { cat "$TMP/root.log"; fail "control: the workspace root's [toolchain] is not reported"; }
    ( cd pinned && "$MCPP" toolchain list > "$TMP/pinned.log" 2>&1 ) || true
    grep -q "$LEGEND" "$TMP/pinned.log" \
        || { cat "$TMP/pinned.log"; fail "control: a member's own [toolchain] is not reported"; }
    ( cd app && "$MCPP" toolchain list > "$TMP/app.log" 2>&1 ) || true
    grep -q "$LEGEND" "$TMP/app.log" \
        || { cat "$TMP/app.log"; fail "a member without [toolchain] does not report the workspace's"; }
    echo "ok: toolchain list in a member reports the workspace's [toolchain]"
fi

# ── pack routing ──────────────────────────────────────────────────────────
( cd app && "$MCPP" pack > "$TMP/pack.log" 2>&1 ) || true
if grep -q "missing required field 'package.version'" "$TMP/pack.log"; then
    cat "$TMP/pack.log"
    fail "pack in a member that leaves version to the workspace was refused"
fi
echo "ok: pack in a member reads the inherited version"

echo "PASS: 773_commands_outside_the_build_read_the_effective_manifest"
