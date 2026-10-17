#!/usr/bin/env bash
# requires: unix-shell python3
# 918 -- a toolchain payload of another packaging revision is used by a build,
# not replaced inside it (design 2026-10-17 D3, D6, D7).
#
# A raised `revision` in the index is a newer packaging, not a broken payload.
# mcpp 2026.10.16.x replaced every payload whose recorded revision was not the
# index's inside the build that read it: a user's next `mcpp run` downloaded
# llvm, gcc (101.7 MB) and glibc (36.7 MB), and on a home with one dangling
# record the replacement failed on every run. Criteria, on the toolchain this
# home builds with, its payload re-recorded at another revision:
#   A. `mcpp new && mcpp build` succeeds and installs, updates or downloads
#      nothing (no `install_packages` call reaches the provider).
#   B. `mcpp toolchain list` says another packaging revision is available.
#   C. `mcpp self doctor` in the project reports the toolchain the build uses
#      and its payload's revision, and starts no install either.
# Fails on 2026.10.16.3: A replaces the payload.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

export MCPP_HOME="$TMP/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
export MCPP_LOG_LEVEL=info
LOG="$MCPP_HOME/log/mcpp.log"

cd "$TMP"
"$MCPP" new probe > new.log 2>&1 || fail "mcpp new failed" new.log
cd probe
"$MCPP" build -v > first.log 2>&1 || fail "the first build failed" first.log

# The payload the build resolved: `Resolved <tc> → <payload>/bin/<driver>`.
COMPILER=$(sed -n 's/.*Resolved [^ ]* → \(.*\)$/\1/p' first.log | tail -1)
[ -n "$COMPILER" ] || { echo "SKIP: the build resolved no managed toolchain payload"; exit 0; }
case "$COMPILER" in @mcpp/*) COMPILER="$MCPP_HOME/${COMPILER#@mcpp/}" ;; esac
PAYLOAD=$(cd "$(dirname "$COMPILER")/.." && pwd)
STAMP="$PAYLOAD/.xpkg-install.json"
[ -f "$STAMP" ] || { echo "SKIP: $PAYLOAD carries no install record"; exit 0; }

# The payload in this home becomes a directory of its own whose children link
# to the inherited one, with a record of its own: the inherited payload (the
# developer's or the runner's) is never written.
if [ -L "$PAYLOAD" ]; then
    SRC=$(readlink -f "$PAYLOAD")
    rm "$PAYLOAD"
    mkdir "$PAYLOAD"
    for child in "$SRC"/* "$SRC"/.[!.]*; do
        [ -e "$child" ] || continue
        name=$(basename "$child")
        [ "$name" = ".xpkg-install.json" ] && continue
        ln -s "$child" "$PAYLOAD/$name"
    done
    cp "$SRC/.xpkg-install.json" "$STAMP"
fi
python3 - "$STAMP" <<'PY' > "$TMP/revision"
import json, sys
path = sys.argv[1]
stamp = json.load(open(path))
recorded = int(stamp.get("revision", 0))
other = recorded - 1 if recorded > 0 else recorded + 1
stamp["revision"] = other
open(path, "w").write(json.dumps(stamp, indent=2) + "\n")
print(other)
PY
OTHER=$(cat "$TMP/revision")
echo "ok: $PAYLOAD re-recorded at packaging revision $OTHER"

# ── A ─────────────────────────────────────────────────────────────────
rm -f "$LOG"
cd "$TMP"
"$MCPP" new probe2 > new2.log 2>&1 || fail "A: mcpp new failed" new2.log
cd probe2
"$MCPP" build > a.log 2>&1 || fail "A: the build failed" a.log
if grep -Eq "Updating|Reinstalling|Downloading|Installing" a.log; then
    fail "A: the build replaced or installed something" a.log
fi
if [ -f "$LOG" ] && grep -q "install_packages" "$LOG"; then
    fail "A: an install reached the provider" "$LOG"
fi
python3 - "$STAMP" "$OTHER" <<'PY' || fail "A: the payload's record changed" "$STAMP"
import json, sys
assert int(json.load(open(sys.argv[1])).get("revision", 0)) == int(sys.argv[2])
PY
echo "ok: A. the build used packaging revision $OTHER and installed nothing"

# ── B ─────────────────────────────────────────────────────────────────
"$MCPP" toolchain list > b.log 2>&1 || fail "B: toolchain list failed" b.log
grep -q "packaging revision .* available\|below the index's floor" b.log \
    || fail "B: toolchain list does not say another packaging is available" b.log
echo "ok: B. toolchain list names the other packaging"

# ── C ─────────────────────────────────────────────────────────────────
rm -f "$LOG"
"$MCPP" self doctor > c.log 2>&1 || true
grep -q "what a build here uses" c.log || fail "C: doctor does not report the build's toolchain" c.log
grep -q "packaging revision $OTHER" c.log || fail "C: doctor does not report the payload's revision" c.log
if [ -f "$LOG" ] && grep -q "install_packages" "$LOG"; then
    fail "C: doctor started an install" "$LOG"
fi
echo "ok: C. doctor reports the toolchain and its payload, and installs nothing"
