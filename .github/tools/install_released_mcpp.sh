#!/usr/bin/env bash
# install_released_mcpp.sh — make a specific PUBLISHED mcpp be the binary that
# the bare `mcpp` shim actually runs, and prove it.
#
# Sibling of install_pinned_mcpp.sh. That one installs the BOOTSTRAP mcpp the
# repo's .xlings.json pins (for self-host builds); this one installs the
# VERSION UNDER TEST for the fresh-install matrix. Same discipline, opposite
# source of truth — and the reason they are two scripts rather than one flag is
# that confusing the two is precisely what broke ci-fresh-install.
#
# THREE THINGS THIS DOES THAT THE INLINE VERSION DID NOT
#
# 1. NEUTRALISE THE REPO'S WORKSPACE PIN.
#
#    This repo's .xlings.json declares `workspace.mcpp` — the bootstrap version,
#    hand-maintained and DELIBERATELY lagging the newest release. It is scoped
#    to the working directory, and inside a checkout it beats anything installed
#    globally. So the fresh-install jobs, which check out the repo first and
#    then run `mcpp` from it, were resolving the bootstrap version instead of
#    the version under test — and failing outright, because only the latter was
#    installed:
#
#        ✓ 1 package(s) installed
#        [error] xlings: version '2026.8.3.2' not found for 'mcpp'
#        [error]   available: 2026.8.3.4
#
#    Every job in the matrix died there, on every run after every release, since
#    "newest release != bootstrap pin" is the normal state.
#
#    ci-aarch64-fresh-install.yml avoids this by ordering the checkout LAST.
#    That does not work here: the `build mcpp` steps run `mcpp clean && mcpp run`
#    INSIDE the repo, so a checkout must be present while mcpp is invoked. The
#    pin has to go instead — this workflow tests the released binary, and the
#    bootstrap pin has no standing in that question.
#
# 2. ACTIVATE, NOT JUST INSTALL (`-u`).
#
#    `xlings install` reports success for "the bytes are on disk", which is not
#    the same claim as "`mcpp` now runs it". install_pinned_mcpp.sh already
#    documents this ("the piece a plain install leaves alone, and the reason CI
#    could install one version and then run another"); this path never got it.
#
# 3. WAIT FOR THE INDEX THE JOB ACTUALLY USES.
#
#    The workflow's wait-index job polls the index's GIT source
#    (raw.githubusercontent.com/openxlings/xim-pkgindex). Jobs install from the
#    PUBLISHED ARTIFACT (xlings-res/xim-index → pointer → tarball), which lags
#    git by however long Publish Index Artifact plus release-CDN propagation
#    takes. Measured on the 2026.8.3.5 release: wait-index reported ready and
#    every job then failed with
#
#        [error] package 'mcpp@2026.8.3.5' not found
#
#    A guard that measures a channel nobody installs from is not a guard. The
#    retry below closes it from the consumer side, which also covers per-edge
#    CDN skew that no central check can see: the runner that polled is not the
#    runner that installs.
#
# Usage:  bash .github/tools/install_released_mcpp.sh <version> [repo_dir]
# stdout: the resolved binary path; diagnostics go to stderr.
set -euo pipefail

VER="${1:?usage: install_released_mcpp.sh <version> [repo_dir]}"
REPO_DIR="${2:-$(pwd)}"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) XL_HOME="${USERPROFILE:-$HOME}"; EXE=".exe" ;;
  *)                    XL_HOME="$HOME";                 EXE=""     ;;
esac

# Address xlings by path, not through PATH: this runs as a child bash, and on
# Windows MSYS re-derives PATH from the Windows environment on startup, dropping
# the mixed-separator entry the caller exported. Same reasoning (and same
# location) as install_pinned_mcpp.sh.
XL="$XL_HOME/.xlings/subos/default/bin/xlings${EXE}"
[ -x "$XL" ] || XL="$XL_HOME/.xlings/subos/current/bin/xlings${EXE}"
[ -x "$XL" ] || XL=$(command -v "xlings${EXE}" 2>/dev/null || true)
[ -n "$XL" ] && [ -x "$XL" ] || {
  echo "FAIL: no xlings under $XL_HOME/.xlings nor on PATH" >&2; exit 1; }

# ── 1. the repo's workspace pin must not decide what we are testing ──────────
if [ -f "$REPO_DIR/.xlings.json" ]; then
  echo "note: removing $REPO_DIR/.xlings.json for this job — it pins the BOOTSTRAP" >&2
  echo "      mcpp, which would override the version under test inside this checkout." >&2
  rm -f "$REPO_DIR/.xlings.json"
fi

# ── 2. install, retrying while the index has not caught up ───────────────────
# Bounded. A miss that is NOT index lag (a typo'd version, a withdrawn release)
# must not cost ten minutes, so the loop reports every attempt and the message
# says which of the two it is on the last one.
attempts="${MCPP_INSTALL_ATTEMPTS:-20}"
delay="${MCPP_INSTALL_RETRY_SECONDS:-30}"
installed=0
for i in $(seq 1 "$attempts"); do
  "$XL" update >/dev/null 2>&1 || true
  if "$XL" install "mcpp@${VER}" -y -g -u >&2; then
    installed=1
    break
  fi
  if [ "$i" -lt "$attempts" ]; then
    echo "note: mcpp@${VER} not installable yet (attempt $i/$attempts) — the published" >&2
    echo "      index artifact may not have propagated; retrying in ${delay}s" >&2
    sleep "$delay"
  fi
done
[ "$installed" = 1 ] || {
  echo "FAIL: could not install mcpp@${VER} after $attempts attempts." >&2
  echo "      Either the index never published it (check the xim-pkgindex bump PR)" >&2
  echo "      or the version does not exist." >&2
  exit 1; }

# ── 3. prove the shim resolves it ────────────────────────────────────────────
# `install` succeeding means the bytes landed, not that `mcpp` runs them, and
# every later step in these jobs invokes the bare shim. Asserting here is what
# turns any future ambient redirection — a workspace pin, a stale xvm
# activation, a PATH surprise — into a named failure instead of a matrix that
# silently tests the wrong binary and reports green.
# Resolve it the way the JOB will: through PATH. That is the entire point of
# the assertion — the steps after this one type `mcpp`, so `mcpp` is what has
# to be checked. Probing a guessed install path instead would verify a binary
# nobody runs, and would happily pass while PATH pointed somewhere else.
# (The known locations are only a fallback for a PATH that is not exported
# yet; `subos/current` and `subos/default` are both in use across the jobs.)
MCPP=$(command -v "mcpp${EXE}" 2>/dev/null || true)
for cand in "$XL_HOME/.xlings/subos/current/bin/mcpp${EXE}" \
            "$XL_HOME/.xlings/subos/default/bin/mcpp${EXE}"; do
  [ -n "$MCPP" ] && break
  [ -x "$cand" ] && MCPP="$cand"
done
[ -n "$MCPP" ] && [ -x "$MCPP" ] || {
  echo "FAIL: mcpp is not on PATH after a successful install" >&2; exit 1; }

probe() { "$MCPP" --version 2>/dev/null | head -1 \
          | grep -oE '[0-9]+(\.[0-9]+)+' | head -1 || true; }

GOT=$(probe)
if [ "$GOT" != "$VER" ]; then
  # `-u` is install-time activation; `xlings use` is the explicit switch, and
  # is what xlings itself suggests when an install leaves the shim behind
  # ("installed, but 'mcpp' still resolves to X — `xlings use mcpp X` to
  # switch"). Doing both is belt and braces, not redundancy: they are two
  # different code paths in xlings and only one of them is load-bearing here.
  # The assertion below stays final either way — this completes the
  # activation, it does not excuse a failure to activate.
  echo "note: shim reported '${GOT:-?}' after install; switching explicitly" >&2
  "$XL" use mcpp "$VER" >&2 2>/dev/null || true
  GOT=$(probe)
fi

[ "$GOT" = "$VER" ] || {
  echo "FAIL: mcpp resolves to '${GOT:-?}' but the version under test is '$VER'" >&2
  echo "hint: something is redirecting the shim. A .xlings.json workspace pin in" >&2
  echo "      the working directory is the usual cause (this script removes the" >&2
  echo "      repo's own, but a parent directory can carry one too); a stale xvm" >&2
  echo "      activation is the other." >&2
  exit 1; }

echo "version under test: $MCPP ($GOT)" >&2
echo "$MCPP"
