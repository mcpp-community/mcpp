#!/usr/bin/env bash
# resources: machine
# 1039 -- two builds that find the std module missing compile it once (#906 self-review).
#
# The std module of a toolchain and flag set is one entry of the home's build cache, shared by every
# build. Two processes that found it missing compiled into the same files at once: the second clang
# read a std.pcm the first was rewriting (SIGBUS), and a build went on with no std.o (e2e 957 and 987,
# run side by side on one runner). The entry is now built under its own lock.
#
# Criteria: with the entry two projects use removed, the two build at once and both succeed; the entry
# is whole afterwards (its BMI, object and record).
#
# It holds the machine: it removes an entry of the shared home's cache, which no other test may be
# building from at that moment.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python

cd "$TMP"
for p in one two; do "$MCPP" new "$p" > /dev/null 2>&1 || fail "mcpp new $p"; done
(cd one && "$MCPP" build > ../first.log 2>&1) || fail "the first build" first.log

# The entry the first build used: the std module whose record it marked last.
home="${MCPP_HOME:-$HOME/.mcpp}"
entry="$("$PY" - "$home/build-cache/v1/std" <<'PY'
import os, sys
root = sys.argv[1]
records = [os.path.join(root, d, "std-module.json") for d in os.listdir(root)] if os.path.isdir(root) else []
records = [r for r in records if os.path.isfile(r)]
print(os.path.dirname(max(records, key=os.path.getmtime)) if records else "")
PY
)"
[ -n "$entry" ] || fail "no std module entry under $home/build-cache/v1/std" first.log
rm -rf "$entry"

(cd one && "$MCPP" build > ../one.log 2>&1) & a=$!
(cd two && "$MCPP" build > ../two.log 2>&1) & b=$!
sa=0; sb=0; wait $a || sa=$?; wait $b || sb=$?
[ "$sa" = 0 ] || fail "the first of two builds at once (exit $sa)" one.log two.log
[ "$sb" = 0 ] || fail "the second of two builds at once (exit $sb)" one.log two.log
[ -f "$entry/std-module.json" ] || fail "the entry has no record afterwards" one.log two.log
ls "$entry"/*.o > /dev/null 2>&1 || fail "the entry has no object afterwards" one.log two.log

echo "PASS: 1039 two builds that find the std module missing compile it once"
