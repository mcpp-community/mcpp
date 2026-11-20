#!/usr/bin/env bash
# Tests that override $MCPP_HOME for isolation (fresh BMI cache / git cache /
# etc.) but still need a working toolchain should source this. It names the
# user's homes as upstreams, links the indices and copies the config so the
# default toolchain pin resolves.
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

# THE DEVELOPER'S HOMES ARE UPSTREAMS OF THE TEST'S HOME, NEVER LINKS INTO IT
# (mcpp#293, design 2026-11-20 §5.10). Each payload linked into the test's home
# made the developer's store part of it: a hook the test's home ran again (gcc's
# specs, an ELF patch) wrote through the link into the developer's toolchain,
# with paths of the test's temporary home, and an install landed in the
# developer's registry. An upstream is read by link at install and never written
# (docs/25 §3); the test's home installs what it needs from it, and registers
# the programs in its own subos, so no subos is copied either.
name_upstreams() {
    local ups=() u list="" config="$MCPP_HOME/config.toml"
    for u in "$USER_MCPP" "$HOME/.xlings" ${USERPROFILE:+"$USERPROFILE/.xlings"}; do
        [[ -d "$u" ]] && ups+=("\"$(cd "$u" && pwd -P)\"")
    done
    [[ ${#ups[@]} -gt 0 ]] || return 0
    list="upstream = [$(IFS=,; echo "${ups[*]}" | sed 's/,/, /g')]"
    touch "$config"
    # The line goes into the [xlings] table, replacing an upstream it had.
    awk -v line="$list" '
        /^\[/ { if (inx && !done) { print line; done = 1 } inx = ($0 ~ /^\[xlings\]/) }
        inx && /^upstream[ \t]*=/ { next }
        { print }
        END { if (inx && !done) { print line; done = 1 } if (!done) { print ""; print "[xlings]"; print line } }
    ' "$config" > "$config.tmp" && mv "$config.tmp" "$config"
}

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
if [[ "${MCPP_INHERIT_CONFIG:-1}" != "0" && -f "$USER_MCPP/config.toml" ]]; then
    cp -f "$USER_MCPP/config.toml" "$MCPP_HOME/config.toml" 2>/dev/null || true
fi
name_upstreams
if [[ -d "$USER_MCPP/bin" ]]; then
    mkdir -p "$MCPP_HOME"
    [[ -e "$MCPP_HOME/bin" ]] \
        || ln -sf "$USER_MCPP/bin" "$MCPP_HOME/bin" 2>/dev/null \
        || cp -r "$USER_MCPP/bin" "$MCPP_HOME/bin"
fi
