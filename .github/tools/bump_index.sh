#!/usr/bin/env bash
# Ask openxlings/xim-pkgindex to propose <project> <version>, and wait for the PR.
#
# The index edits its own recipes. Since openxlings/xim-pkgindex#1027 one
# updater does it for every package (`xpkgindex-update.yml`, pinned
# openxlings/xpkgindex-update): it discovers the release from the recipe's
# `extensions.autoupdate.source` (xlings-res/<project> releases), requires the
# GLOBAL and CN resources, computes each sha256 and opens a PR whose body
# starts with `<!-- xpkgindex-update: {"id":"xim:<project>","candidate":"<version>"} -->`
# (its branch, `auto/bump-xim-<project>`, does not name the version, so the
# marker is what identifies the proposal). The script that used to do this here,
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
#   PKGINDEX_TOKEN (required) -- a token with Actions: write on
#   openxlings/xim-pkgindex (a workflow dispatch needs that permission;
#   Contents and Pull requests do not grant it)
set -euo pipefail

PROJ="${1:?usage: bump_index.sh <project> <version>}"
VER="${2:?usage: bump_index.sh <project> <version>}"
INDEX_REPO="${INDEX_REPO:-openxlings/xim-pkgindex}"
export MARKER="<!-- xpkgindex-update: {\"id\":\"xim:${PROJ}\",\"candidate\":\"${VER}\"} -->"
WAIT_S="${BUMP_WAIT_S:-1200}"
export GH_TOKEN="${PKGINDEX_TOKEN:?PKGINDEX_TOKEN required (dispatch on $INDEX_REPO)}"

info() { echo "[bump] $*"; }

# The updater's PR for this version, open or merged.
pr_url() {
  gh pr list -R "$INDEX_REPO" --state all --limit 50 --json url,body,state \
    --jq '[.[] | select(.state != "CLOSED" and (.body | startswith(env.MARKER)))][0].url // empty' \
    2>/dev/null || true
}

if url="$(pr_url)" && [[ -n "$url" ]]; then
  info "PR already proposed: $url"
  exit 0
fi

info "dispatching xpkgindex-update.yml for xim:$PROJ"
if ! gh workflow run xpkgindex-update.yml -R "$INDEX_REPO" --ref main -f "package=xim:$PROJ"; then
  echo "[bump] PKGINDEX_TOKEN cannot dispatch on $INDEX_REPO (it needs Actions: write);" \
       "a maintainer runs: gh workflow run xpkgindex-update.yml -R $INDEX_REPO --ref main -f package=xim:$PROJ" >&2
  exit 1
fi

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
echo "[bump] no PR for xim:$PROJ $VER after ${WAIT_S}s; see" \
     "https://github.com/$INDEX_REPO/actions/workflows/xpkgindex-update.yml" >&2
exit 1
