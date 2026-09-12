#!/usr/bin/env bash
# requires: elf gcc
# 652_an_application_is_a_kind.sh -- `kind = "app"` (design record #622 A3,
# 2026-09-12-622-a-ui-framework-on-android-ios-and-web.md §2.3).
#
# An application is "the thing a user launches", on every row. Its link form
# is a property of the ROW, not of the manifest: on every row this script can
# exercise directly (an ELF host) it is identical to `bin`; the Android row,
# where it is a shared object instead, is 652b (`# requires: android-ndk`).
#
# OLDER-ENGINE COMPATIBILITY, BY READING RATHER THAN BY RUNNING (§1 rule 6,
# T3 step 7): `mcpp 2026.9.12.2`'s `toml.cppm:1110` reads
#
#   targets.<name>.kind must be 'bin', 'lib' or 'shared'; got '<value>'
#
# -- three kinds. Verified by hand against that exact binary
# (~/.xlings/data/xpkgs/xim-x-mcpp/2026.9.12.2/bin/mcpp) while writing this
# script: `kind = "app"` is refused, naming three kinds, which is correct --
# an application manifest is a root, and a root that names a form an older
# engine cannot produce must not build. The new refusal lists FOUR:
# 'bin', 'app', 'lib' or 'shared' -- asserted below on the engine under test.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
mkdir -p src

cat > mcpp.toml <<'TOML'
[package]
name    = "myapp"
version = "0.1.0"

[targets.myapp]
kind = "app"
main = "src/main.cpp"

[targets.tool]
kind = "bin"
main = "src/tool.cpp"
TOML

cat > src/main.cpp <<'CPP'
#include <cstdio>
int main() { std::puts("APP-RUN"); return 0; }
CPP
cat > src/tool.cpp <<'CPP'
#include <cstdio>
int main() { std::puts("TOOL-RUN"); return 0; }
CPP

# ── 1. The refusal, on the engine under test, names four kinds ─────────────
cat > bad.toml <<'TOML'
[package]
name    = "p"
version = "0.1.0"

[targets.t]
kind = "framework"
TOML
mkdir -p bad/src
cp bad.toml bad/mcpp.toml
printf 'int main(){}\n' > bad/src/main.cpp
out=$( cd bad && "$MCPP" build 2>&1 ) && fail "an unrecognized kind must be refused" <(echo "$out")
grep -q "must be 'bin', 'app', 'lib' or 'shared'" <<<"$out" \
    || fail "the refusal does not list all four kinds" <(echo "$out")
echo "unrecognized kind lists four kinds OK"

# ── 2. The host build: both targets link, `app` == `bin` off Android ───────
"$MCPP" build > build.log 2>&1 || fail "the host build failed" build.log
outdir=$(dirname "$(ls -t target/*/*/build.ninja | head -1)")
[ -x "$outdir/bin/myapp" ] || fail "bin/myapp was not linked" build.log
[ -x "$outdir/bin/tool" ]  || fail "bin/tool was not linked" build.log
file "$outdir/bin/myapp" | grep -qi "executable" \
    || fail "bin/myapp is not an executable (an app's host form must be one)" build.log
echo "host build links app and bin as executables OK"

# ── 3. `mcpp run` of an app whose form here IS an executable just runs ─────
out=$("$MCPP" run myapp 2>&1) || fail "mcpp run myapp failed on the host" <(echo "$out")
grep -q "APP-RUN" <<<"$out" || fail "mcpp run myapp did not print its marker" <(echo "$out")
echo "mcpp run myapp OK"

# Negative-direction control for (3): a target that is genuinely absent is
# still refused as before -- the app-aware run path must not swallow that.
out=$("$MCPP" run nosuchtarget 2>&1) && fail "mcpp run of a nonexistent target must fail" <(echo "$out")
grep -q "no binary target 'nosuchtarget' found" <<<"$out" \
    || fail "the generic no-binary-target refusal changed wording" <(echo "$out")
echo "mcpp run of a nonexistent target still refused generically OK"

# ── 4. `bin` is unaffected: it still runs like it always has ───────────────
out=$("$MCPP" run tool 2>&1) || fail "mcpp run tool failed" <(echo "$out")
grep -q "TOOL-RUN" <<<"$out" || fail "mcpp run tool did not print its marker" <(echo "$out")
echo "mcpp run tool (bin) unaffected OK"

# ── 5. `mcpp pack` treats `app` as the program route, same as `bin` ────────
"$MCPP" pack --format dir > pack.log 2>&1 || fail "mcpp pack failed" pack.log
staged=$(ls -d target/dist/myapp-0.1.0-*/ 2>/dev/null | head -1)
[ -n "$staged" ] || fail "no staged tree under target/dist" pack.log
[ -x "${staged}bin/myapp" ] || fail "the staged tree has no bin/myapp" pack.log
echo "mcpp pack routes an app as a program OK"

echo "652: kind = \"app\" OK"
