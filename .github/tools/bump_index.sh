#!/usr/bin/env bash
# Ask openxlings/xim-pkgindex to propose <project> <version>, and wait for the PR.
#
# The index edits its own recipes. Since openxlings/xim-pkgindex#1027 one
# updater does it for every package (`xpkgindex-update.yml`, pinned
# openxlings/xpkgindex-update): it discovers the release from the recipe's
# `extensions.autoupdate.source` (xlings-res/<project> releases), requires the
# GLOBAL and CN resources, computes each sha256 and opens
# `auto/bump-xim-<project>-<version>`. The script that used to do this here,
# `.github/scripts/version-check.py`, was removed from the index by that
# change; 2026.10.24.1 is the release that found out (publish-ecosystem
# failed on the missing file). Asking the index's own updater keeps one writer
# of the recipe.
#
# This opens a PR only -- a maintainer reviews and merges it. It runs after
# the binaries are mirrored, because the updater requires both hosts.
#
# Usage: bump_index.sh <project> <version>       # e.g. mcpp 2026.10.24.1
# Auth:
#   PKGINDEX_TOKEN (required) -- may dispatch workflows of openxlings/xim-pkgindex
set -euo pipefail

PROJ="${1:?usage: bump_index.sh <project> <version>}"
VER="${2:?usage: bump_index.sh <project> <version>}"
INDEX_REPO="${INDEX_REPO:-openxlings/xim-pkgindex}"
BRANCH="auto/bump-xim-${PROJ}-${VER}"
WAIT_S="${BUMP_WAIT_S:-1200}"
export GH_TOKEN="${PKGINDEX_TOKEN:?PKGINDEX_TOKEN required (dispatch on $INDEX_REPO)}"

info() { echo "[bump] $*"; }

pr_url() { gh pr view "$BRANCH" -R "$INDEX_REPO" --json url --jq .url 2>/dev/null || true; }

if url="$(pr_url)" && [[ -n "$url" ]]; then
  info "PR already open: $url"
  exit 0
fi

info "dispatching xpkgindex-update.yml for xim:$PROJ"
gh workflow run xpkgindex-update.yml -R "$INDEX_REPO" --ref main -f "package=xim:$PROJ"

deadline=$(( $(date +%s) + WAIT_S ))
while (( $(date +%s) < deadline )); do
  sleep 30
  if url="$(pr_url)" && [[ -n "$url" ]]; then
    info "PR opened: $url"
    exit 0
  fi
done

# BLOCKING: without the PR the index never learns the release exists. The
# updater's run says why (a resource missing on one host, a policy refusal).
echo "[bump] no $BRANCH after ${WAIT_S}s; see" \
     "https://github.com/$INDEX_REPO/actions/workflows/xpkgindex-update.yml" >&2
exit 1
