#!/usr/bin/env bash
# Dev binaries are built under build/.../release/mcpp, not <root>/bin/mcpp.
# With MCPP_HOME unset they should use the conventional ~/.mcpp sandbox;
# only release-style <root>/bin/mcpp should self-locate to <root>.
set -e

out=$(env -u MCPP_HOME "$MCPP" self env 2>&1)
echo "$out" | grep -q "MCPP_HOME *= *$HOME/.mcpp" || {
    echo "dev binary should default to $HOME/.mcpp"; echo "$out"; exit 1; }

echo "OK"
