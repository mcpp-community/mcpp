#!/usr/bin/env bash
# slim_linux_payload.sh — strip the shipped ELF binaries in a staged release
# wrapper directory, then ASSERT the result.
#
# WHY. The linux tarballs were 34.8MB (x86_64) and 30.2MB (aarch64) while the
# macOS one was 6.1MB and the Windows one 4.2MB. Almost none of that was mcpp:
# the vendored `registry/bin/xlings` shipped at 97.3MB / 86.9MB with full debug
# info, against a 4.3MB / 2.8MB mcpp. Stripping both takes the x86_64 tarball
# from 34.81MB to 4.62MB — 7.5x — with the stripped binaries verified working
# (`--version`, `new`, `build`, `run`).
#
# That size is what makes the GitCode mirror leg fail: measured upload from a
# GitHub-hosted US runner to file.gitcode.com (a single Huawei Cloud origin in
# Beijing) is ~0.012 MB/s in the inbound-to-CN direction — the SAME runner
# downloads from that host at 3.87 MB/s and uploads to GitHub at 16 MB/s, and a
# mainland-CN host reaches 1.84 MB/s to the same endpoint. At 0.012 MB/s a
# 34.8MB asset needs ~45 minutes; at 4.6MB it needs ~6.
#
# ASSERT, don't trust. release.yml already stripped the x86_64 mcpp before
# packing — and shipped it unstripped anyway, because `mcpp pack` rebuilds the
# binary and overwrites the stripped one. A `strip` whose effect is never
# checked is a comment, not a step. Hence: this runs on the STAGED payload
# (after pack, before tar) and fails loudly if anything is still unstripped.
#
# Linux only, on purpose. The macOS and Windows payloads are already small, and
# stripping a Mach-O invalidates its (ad-hoc) code signature — not worth the
# risk for ~2MB when 100% of the mirror problem is the two linux tarballs.
#
# Usage: slim_linux_payload.sh <wrapper-dir> [strip-cmd]
set -euo pipefail

DIR="${1:?usage: slim_linux_payload.sh <wrapper-dir> [strip-cmd]}"
STRIP="${2:-strip}"

[ -d "$DIR" ] || { echo "slim: no such directory: $DIR" >&2; exit 1; }
command -v "$STRIP" >/dev/null 2>&1 || [ -x "$STRIP" ] || {
  echo "slim: strip tool not usable: $STRIP" >&2; exit 1; }

size_of() { stat -c %s "$1" 2>/dev/null || stat -f %z "$1"; }
mb() { awk -v b="$1" 'BEGIN{printf "%.1f", b/1048576}'; }

# The two binaries the release actually ships. Both are static ELFs; a missing
# one is not an error (the aarch64 leg skips xlings when the upstream download
# fails, and that has its own handling), but a PRESENT one must end up stripped.
total_before=0 total_after=0 found=0
for rel in bin/mcpp registry/bin/xlings; do
  f="$DIR/$rel"
  [ -f "$f" ] || { echo "slim: $rel absent, skipping"; continue; }
  found=$((found + 1))
  before=$(size_of "$f")
  "$STRIP" --strip-unneeded "$f" 2>/dev/null || "$STRIP" "$f"
  after=$(size_of "$f")
  total_before=$((total_before + before))
  total_after=$((total_after + after))
  echo "slim: $rel  $(mb "$before")MB -> $(mb "$after")MB"

  # The assertion this script exists for.
  if file "$f" | grep -q 'not stripped'; then
    file "$f"
    echo "slim: FAIL: $rel is still not stripped after running $STRIP" >&2
    exit 1
  fi
  # A stripped binary that no longer runs is worse than a fat one. The callers
  # smoke-test the packaged tarball too, but catch it here where the failure
  # names the file.
  [ -x "$f" ] || { echo "slim: FAIL: $rel lost its exec bit" >&2; exit 1; }
done

[ "$found" -gt 0 ] || { echo "slim: FAIL: no shippable binary found under $DIR" >&2; exit 1; }
echo "slim: payload $(mb "$total_before")MB -> $(mb "$total_after")MB across $found binary(ies)"
