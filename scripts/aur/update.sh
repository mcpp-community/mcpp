#!/usr/bin/env bash
# Compatibility entrypoint for rendering the mcpp-bin AUR package.
#
# Desired state comes only from the latest complete stable release's immutable
# mcpp-release.json.  An optional VERSION or vVERSION must match that exact
# release; this wrapper has no downgrade override and publishes nothing.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if (( $# > 1 )); then
    echo "usage: scripts/aur/update.sh [VERSION|vVERSION]" >&2
    exit 2
fi

args=(
    --render-only
    --output-dir "$SCRIPT_DIR/mcpp-bin"
    --trigger local-update
)

if (( $# == 1 )); then
    tag=$1
    [[ $tag == v* ]] || tag="v$tag"
    args+=(--tag "$tag")
fi

# Offline/release-audit callers may inject an already-downloaded immutable
# manifest and its validated payload/sidecar directory.  Both are required.
if [[ -n ${MCPP_AUR_MANIFEST:-} || -n ${MCPP_AUR_ASSETS_DIR:-} ]]; then
    [[ -n ${MCPP_AUR_MANIFEST:-} && -n ${MCPP_AUR_ASSETS_DIR:-} ]] || {
        echo "MCPP_AUR_MANIFEST and MCPP_AUR_ASSETS_DIR must be set together" >&2
        exit 2
    }
    args+=(--manifest "$MCPP_AUR_MANIFEST" --assets-dir "$MCPP_AUR_ASSETS_DIR")
fi

exec python3 "$SCRIPT_DIR/reconcile_mcpp_bin.py" "${args[@]}"
