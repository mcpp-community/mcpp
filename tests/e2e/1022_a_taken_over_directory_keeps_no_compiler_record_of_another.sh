#!/usr/bin/env bash
# requires: gcc
# 1022 -- a build directory taken over keeps no compiler record of another
# directory (2026.11.7.1). A split compile (`bmi_schedule`, GCC's detach-codegen)
# keeps its slot's records beside each object (`<object>.sched.*`), and the
# completion certificate names the object by its absolute path. A new
# fingerprint of a configuration's lineage takes its predecessor's directory
# over (design 2026-10-31 §8), and the certificates there named the
# predecessor's objects: the next build refused them at admission ("cached
# compiler certificate does not name its own object"), and every build of the
# configuration after an mcpp upgrade failed so, until `mcpp clean`.
#
# The predecessor is made with plain files: a dev build's directory, renamed,
# whose `lineage` is the release configuration's, so the release build takes it
# over while its certificates name the dev build's objects.
#
# Criteria:
#   A. The release build takes the predecessor over and builds.
#   B. No record of the predecessor is left beside the objects.
#   C. A certificate of another directory found at admission -- the directory
#      moved, or restored elsewhere -- is no record: the build compiles the
#      object again rather than failing.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
mkdir -p app/src && cd app
printf '[package]\nname    = "app"\nversion = "0.1.0"\n\n[build]\nbmi_schedule = "on"\n' > mcpp.toml
printf 'export module greet;\nexport int answer() { return 42; }\n' > src/greet.cppm
printf 'import greet;\nint main() { return answer() == 42 ? 0 : 1; }\n' > src/main.cpp
config() { ls -d target/*/*/ | grep -E '/[0-9a-f]{16}/$' | head -1 | sed 's#/$##'; }

"$MCPP" build --release > r0.log 2>&1 || fail "the first release build failed" r0.log
release=$(config); cp "$release/lineage" release.lineage; rm -rf "$release"
"$MCPP" build > d0.log 2>&1 || fail "the dev build failed" d0.log
dev=$(config)
ls "$dev"/obj/*.sched.completed > /dev/null 2>&1 || fail "the split compile left no certificate" <(find "$dev/obj")
pred="$(dirname "$dev")/0000000000000000"
mv "$dev" "$pred"; cp release.lineage "$pred/lineage"

# A
printf 'import greet;\nint main() { return answer() == 42 ? 0 : 2; }\n' > src/main.cpp
"$MCPP" build --release > a.log 2>&1 || fail "the build that took the predecessor over failed" a.log
[ ! -d "$pred" ] || fail "the predecessor was not taken over" <(ls "$(dirname "$pred")")
taken=$(config)
echo "ok: the release build took the predecessor over and built"

# B
if grep -rl "/0000000000000000/" "$taken"/obj --include='*.sched*' > /dev/null 2>&1; then
    fail "a record of the predecessor was left beside the objects" <(grep -rl "/0000000000000000/" "$taken"/obj)
fi
echo "ok: no record of the predecessor is left"

# C
moved="$TMP/moved-app"; cp -a "$TMP/app" "$moved"; cd "$moved"
printf 'export module greet;\nexport int answer() { return 41 + 1; }\n' > src/greet.cppm
"$MCPP" build --release > c.log 2>&1 || fail "the build of a moved directory failed" c.log
echo "ok: a certificate of another directory is no record"

echo "PASS: 1022 a taken over directory keeps no compiler record of another"
