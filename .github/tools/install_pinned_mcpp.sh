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

"$XL" install -y -u -g >&2

MCPP="$XL_HOME/.xlings/subos/default/bin/mcpp${EXE}"
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
PIN=$(grep -oE '"mcpp"[[:space:]]*:[[:space:]]*"[^"]+"' "$REPO_DIR/.xlings.json" \
      | grep -oE '[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1 || true)
GOT=$("$MCPP" --version 2>/dev/null | head -1 \
      | grep -oE '[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1 || true)
# Exact comparison: a substring match would let a pin of 0.0.10 be satisfied by
# a 0.0.109 binary.
[ -n "$PIN" ] && [ "$GOT" = "$PIN" ] || {
  echo "FAIL: .xlings.json pins '${PIN:-?}' but the active mcpp is '${GOT:-?}'" >&2
  exit 1; }
echo "bootstrap mcpp: $MCPP ($GOT)" >&2

echo "$MCPP"
