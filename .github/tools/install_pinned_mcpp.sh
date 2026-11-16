#!/usr/bin/env bash
# install_pinned_mcpp.sh — install the bootstrap mcpp that .xlings.json pins,
# and print the resolved binary's path on stdout.
#
# WHY THIS EXISTS. Six places bootstrapped mcpp with `xlings install mcpp -y`.
# Passing the package name is what breaks it: with an explicit name xlings
# resolves "newest in this runner's index copy" and the workspace pin never
# applies. On 2026-07-26 CI jobs were running 0.0.102, 0.0.105 and 0.0.107
# against an index whose floor is 0.0.108. Warm caches hid it; the moment one
# went cold the floor check made EVERY descriptor read return nothing, every
# dependency fell back to its legacy derived address, and the build died on
# `mcpplibs.cmdline@0.0.1` — an error naming neither the version nor the floor.
#
# WHAT THIS DOES NOT DO. It does not parse the pin to decide what to install,
# does not refresh the index, and does not go looking for the binary on disk.
# `.xlings.json` is xlings' own file and xlings already does all three:
#
#     $ xlings install                 # no package name
#     [xlings] 'mcpp@0.0.108' not in current index; refreshing index...
#       Packages to install (1):
#         ◆ xim:mcpp@0.0.108
#
# An earlier version of this script re-implemented every one of those steps by
# hand — grep the pin, `xlings update` on miss, `find` the install directory.
# That is the same mistake the fix it supports is about: re-deriving what the
# owner of the information will tell you. It also cost two bugs (a hardcoded
# per-platform layout, and a `find` that exits non-zero on one unreadable
# directory under `set -eo pipefail`).
#
# `-u` activates the version just installed, so the shim resolves to it; that
# is the piece a plain `install` leaves alone, and the reason CI could install
# one version and then run another.
#
# Usage:  MCPP=$(bash .github/tools/install_pinned_mcpp.sh)
#         MCPP=$(bash .github/tools/install_pinned_mcpp.sh /path/to/repo)
#
# stdout is ONLY the binary path; all diagnostics go to stderr.
set -euo pipefail

REPO_DIR="${1:-$(pwd)}"
[ -f "$REPO_DIR/.xlings.json" ] || {
  echo "FAIL: no .xlings.json at $REPO_DIR" >&2; exit 1; }

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) XL_HOME="${USERPROFILE:-$HOME}"; EXE=".exe" ;;
  *)                    XL_HOME="$HOME";                 EXE=""     ;;
esac

# The workspace pin is read from the CWD, so run from the repo regardless of
# where the caller happens to be (the Windows legs cd into an unpack dir and
# never return).
cd "$REPO_DIR"

# Address xlings by path, not through PATH. The callers export
# `$USERPROFILE/.xlings/subos/default/bin` into PATH before invoking this, and
# `xlings.exe --version` works in that shell — but this script runs as a child
# bash, and MSYS re-derives PATH from the Windows environment on startup, which
# drops that mixed-separator entry (`C:\Users\x/.xlings/...`) and leaves
# `xlings.exe: command not found`. This is the same well-known location every
# caller already hardcodes as XLINGS_BIN, so it adds no new layout knowledge.
XL="$XL_HOME/.xlings/subos/default/bin/xlings${EXE}"
[ -x "$XL" ] || XL=$(command -v "xlings${EXE}" 2>/dev/null || true)
[ -n "$XL" ] && [ -x "$XL" ] || {
  echo "FAIL: no xlings at $XL_HOME/.xlings/subos/default/bin nor on PATH" >&2
  exit 1; }

PIN=$(grep -oE '"mcpp"[[:space:]]*:[[:space:]]*"[^"]+"' "$REPO_DIR/.xlings.json" \
      | grep -oE '[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1 || true)

# XLINGS FIRST, ITS MIRRORS IN TURN, THE RELEASE LAST. A job's xlings comes from
# the cache with the index of when the cache was saved, and xlings keeps that
# index when its refresh fails: during GitHub's incident of 2026-11-09
# ("timeouts due to surge in anonymous users") the anonymous download of the
# index answered 504 and a pin published after the cache was "not found".
#   1-2  the configured mirror (GLOBAL), the index refreshed before the second;
#   3-4  the CN mirror (GitCode first, GitHub after), refreshed; the fourth by
#        git, which xlings otherwise declines for an artifact-managed index;
#   then the release itself (install.sh: GitHub, then GitCode, sha256 checked).
MCPP="$XL_HOME/.xlings/subos/default/bin/mcpp${EXE}"
installed=no
for attempt in 1 2 3 4; do
  case "$attempt" in
    2) "$XL" update >&2 || true ;;
    3) "$XL" config --mirror CN >&2 || true; "$XL" update >&2 || true ;;
    4) XLINGS_INDEX_SOURCE=git "$XL" update >&2 || true ;;
  esac
  if "$XL" install -y -u -g >&2; then installed=yes; break; fi
  sleep $(( attempt * 10 ))
done
"$XL" config --mirror GLOBAL >&2 || true
if [ "$installed" = no ]; then
  echo "::warning::xlings could not install mcpp $PIN; taking the release itself" >&2
  prefix="${RUNNER_TEMP:-$(mktemp -d)}/mcpp-release-$PIN"
  if [ -n "$EXE" ]; then
    zip="mcpp-$PIN-windows-x86_64.zip"
    mkdir -p "$prefix"
    bash "$REPO_DIR/.github/tools/fetch_release.sh" \
      "https://github.com/mcpp-community/mcpp/releases/download/v$PIN/$zip" "$prefix/$zip" >&2
    (cd "$prefix" && unzip -qo "$zip" && rm -f "$zip")
    MCPP=$(find "$prefix" -type f -name "mcpp.exe" -path '*/bin/*' | head -1)
  else
    MCPP_VERSION="$PIN" MCPP_PREFIX="$prefix" MCPP_NO_PATH=1 bash "$REPO_DIR/install.sh" >&2
    MCPP="$prefix/bin/mcpp"
  fi
  [ -z "${GITHUB_PATH:-}" ] || dirname "$MCPP" >> "$GITHUB_PATH"
fi
[ -x "$MCPP" ] || { echo "FAIL: no mcpp at $MCPP after install" >&2; exit 1; }

# Assert the outcome rather than trust it. This is the ONLY place the pin is
# parsed, and only to check what xlings actually activated — a guard against
# the silent drift this script exists to end, not a second resolution path.
#
# The `|| true` on both captures is load-bearing under `set -eo pipefail`: a
# `| grep` that matches nothing exits 1, which would kill the script silently —
# CI would get a bare non-zero and no diagnostic, exactly the failure mode this
# whole change is meant to stop. A miss has to reach the check below, which can
# name what it expected and what it found.
#
# Three OR four segments: mcpp's version scheme is the date form YYYY.M.D.N
# (e.g. 2026.7.27.1). A three-segment-only pattern still MATCHES such a version
# — it just silently truncates it to "2026.7.27" on both sides, so the
# comparison below would accept any release of that day and this guard would
# quietly stop guarding.
GOT=$("$MCPP" --version 2>/dev/null | head -1 \
      | grep -oE '[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1 || true)
# Exact comparison: a substring match would let a pin of 0.0.10 be satisfied by
# a 0.0.109 binary.
[ -n "$PIN" ] && [ "$GOT" = "$PIN" ] || {
  echo "FAIL: .xlings.json pins '${PIN:-?}' but the active mcpp is '${GOT:-?}'" >&2
  exit 1; }
echo "bootstrap mcpp: $MCPP ($GOT)" >&2

# The package index the commit builds against is the one published now. The
# sandbox the caller restored holds the index as it was when that cache was
# saved, and the pinned mcpp refreshes on a miss only for the root's own
# [dependencies] (fixed in 2026.10.26.2): a version a path member names --
# a member's `os = "0.1.0"` -- missed against it with no refresh.
"$MCPP" index update >&2 || echo "::warning::the bootstrap mcpp could not refresh its package index" >&2

# THE RELEASED BINARY ITSELF, for a program that runs the bootstrap outside
# this checkout (MCPP_BOOT; e2e 772, a released client builds a published
# archive). $MCPP is xlings' shim, which takes the version from the project of
# its working directory and finds none in a directory of a test's own. The
# payload is where xlings places a package, under the home this script found;
# the release fallback above is a binary already.
boot="$MCPP"
payload="$XL_HOME/.xlings/data/xpkgs/xim-x-mcpp/$PIN/bin/mcpp${EXE}"
[ "$installed" = no ] || [ ! -f "$payload" ] || boot="$payload"
[ -z "${GITHUB_ENV:-}" ] || echo "MCPP_BOOT=$boot" >> "$GITHUB_ENV"

echo "$MCPP"
