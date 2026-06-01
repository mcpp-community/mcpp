#!/usr/bin/env bash
# Tests that override $MCPP_HOME for isolation (fresh BMI cache / git cache /
# etc.) but still need a working toolchain should source this. It links the
# user's installed toolchain payloads and copies the config so the default
# toolchain pin resolves.
#
# Usage:    source "$(dirname "$0")/_inherit_toolchain.sh"
#           # ($MCPP_HOME must be set before sourcing)

if [[ -z "${MCPP_HOME:-}" ]]; then
    echo "FATAL: _inherit_toolchain.sh: MCPP_HOME must be set" >&2
    return 1 2>/dev/null || exit 1
fi
mkdir -p "$MCPP_HOME"

# On Windows, HOME may differ from USERPROFILE; try both.
USER_MCPP="${HOME}/.mcpp"
if [[ ! -d "$USER_MCPP" && -n "${USERPROFILE:-}" ]]; then
    USER_MCPP="$USERPROFILE/.mcpp"
fi

link_xpkg_payloads() {
    local source_dir="$1"
    local target_dir="$MCPP_HOME/registry/data/xpkgs"
    [[ -d "$source_dir" ]] || return 0
    mkdir -p "$target_dir"

    local entry base
    shopt -s nullglob
    for entry in "$source_dir"/*; do
        base="$(basename "$entry")"
        [[ -e "$target_dir/$base" ]] && continue
        ln -sf "$entry" "$target_dir/$base" 2>/dev/null \
            || cp -r "$entry" "$target_dir/$base"
    done
    shopt -u nullglob
}

if [[ -d "$USER_MCPP/registry/data/xpkgs" || -d "$HOME/.xlings/data/xpkgs" || ( -n "${USERPROFILE:-}" && -d "$USERPROFILE/.xlings/data/xpkgs" ) ]]; then
    mkdir -p "$MCPP_HOME/registry/data"
    link_xpkg_payloads "$USER_MCPP/registry/data/xpkgs"
    link_xpkg_payloads "$HOME/.xlings/data/xpkgs"
    if [[ -n "${USERPROFILE:-}" ]]; then
        link_xpkg_payloads "$USERPROFILE/.xlings/data/xpkgs"
    fi
fi
if [[ -d "$USER_MCPP/registry/data/xim-pkgindex" ]]; then
    mkdir -p "$MCPP_HOME/registry/data"
    [[ -e "$MCPP_HOME/registry/data/xim-pkgindex" ]] \
        || ln -sf "$USER_MCPP/registry/data/xim-pkgindex" "$MCPP_HOME/registry/data/xim-pkgindex" 2>/dev/null \
        || cp -r "$USER_MCPP/registry/data/xim-pkgindex" "$MCPP_HOME/registry/data/xim-pkgindex"
fi
if [[ -d "$USER_MCPP/registry/data/xim-index-repos" ]]; then
    mkdir -p "$MCPP_HOME/registry/data"
    [[ -e "$MCPP_HOME/registry/data/xim-index-repos" ]] \
        || ln -sf "$USER_MCPP/registry/data/xim-index-repos" "$MCPP_HOME/registry/data/xim-index-repos" 2>/dev/null \
        || cp -r "$USER_MCPP/registry/data/xim-index-repos" "$MCPP_HOME/registry/data/xim-index-repos"
fi
if [[ "${MCPP_INHERIT_SUBOS:-1}" != "0" && -d "$USER_MCPP/registry/subos" ]]; then
    mkdir -p "$MCPP_HOME/registry"
    [[ -e "$MCPP_HOME/registry/subos" ]] \
        || ln -sf "$USER_MCPP/registry/subos" "$MCPP_HOME/registry/subos" 2>/dev/null \
        || cp -r "$USER_MCPP/registry/subos" "$MCPP_HOME/registry/subos"
fi
if [[ "${MCPP_INHERIT_CONFIG:-1}" != "0" && -f "$USER_MCPP/config.toml" ]]; then
    cp -f "$USER_MCPP/config.toml" "$MCPP_HOME/config.toml" 2>/dev/null || true
fi
if [[ -d "$USER_MCPP/bin" ]]; then
    mkdir -p "$MCPP_HOME"
    [[ -e "$MCPP_HOME/bin" ]] \
        || ln -sf "$USER_MCPP/bin" "$MCPP_HOME/bin" 2>/dev/null \
        || cp -r "$USER_MCPP/bin" "$MCPP_HOME/bin"
fi
