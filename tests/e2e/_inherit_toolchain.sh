#!/usr/bin/env bash
# Tests that override $MCPP_HOME for isolation (fresh BMI cache / git cache /
# etc.) but still need a working toolchain should source this. It symlinks
# the user's installed toolchains and copies the config so the default
# toolchain pin resolves.
#
# Usage:    source "$(dirname "$0")/_inherit_toolchain.sh"
#           # ($MCPP_HOME must be set before sourcing)

if [[ -z "${MCPP_HOME:-}" ]]; then
    echo "FATAL: _inherit_toolchain.sh: MCPP_HOME must be set" >&2
    return 1 2>/dev/null || exit 1
fi
mkdir -p "$MCPP_HOME"

USER_MCPP="${HOME}/.mcpp"
if [[ -d "$USER_MCPP/registry/data/xpkgs" ]]; then
    mkdir -p "$MCPP_HOME/registry/data"
    [[ -e "$MCPP_HOME/registry/data/xpkgs" ]] \
        || ln -sf "$USER_MCPP/registry/data/xpkgs" "$MCPP_HOME/registry/data/xpkgs"
fi
if [[ -d "$USER_MCPP/registry/subos" ]]; then
    mkdir -p "$MCPP_HOME/registry"
    [[ -e "$MCPP_HOME/registry/subos" ]] \
        || ln -sf "$USER_MCPP/registry/subos" "$MCPP_HOME/registry/subos"
fi
if [[ -f "$USER_MCPP/config.toml" ]]; then
    cp -f "$USER_MCPP/config.toml" "$MCPP_HOME/config.toml" 2>/dev/null || true
fi
if [[ -d "$USER_MCPP/bin" ]]; then
    mkdir -p "$MCPP_HOME"
    [[ -e "$MCPP_HOME/bin" ]] \
        || ln -sf "$USER_MCPP/bin" "$MCPP_HOME/bin"
fi
