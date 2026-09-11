#!/usr/bin/env bash
# 645_the_fast_path_compares_the_toolchain_request.sh -- the fast path replays a
# recorded build only for the toolchain request that recorded it (T1 of the
# 2026-09-12 engine-gaps record).
#
# Measured before the fix: after `mcpp build` with gcc, `mcpp build --toolchain
# llvm@22.1.8` printed `Finished dev in 0.00s` and left the gcc artefact in
# place. Neither `--toolchain` (which reaches the build as MCPP_TOOLCHAIN) nor
# the machine default (`[toolchain] default` in config.toml) was compared, and
# every resolution-time check was skipped with them.
#
# Asserted on whether toolchain resolution ran, which the fast path skips:
#   A  `mcpp build` twice; the second is the fast path. This is the control:
#      without it the assertions below could not fail.
#   B  the same toolchain requested through `--toolchain`; resolution runs,
#      and MCPP_TOOLCHAIN is then the same request.
#   A  `mcpp build` resolves again, and a further one is the fast path.
# `mcpp run` has a fast path of its own and takes the same A-B-A.
# The machine-default leg sets `[toolchain] default` in an isolated home to a
# second installed version of the same family and asserts that the artefact
# changed compiler. It is reported as not measured when no second version is
# installed.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
resolutions() { grep -c 'Resolving toolchain' "$1" || true; }

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
printf 'int main() { return 0; }\n' > src/main.cpp
cat > mcpp.toml <<'TOML'
[package]
name    = "fastpath"
version = "0.1.0"
TOML

# ── A ───────────────────────────────────────────────────────────────────────
"$MCPP" build > a1.log 2>&1 || fail "the first build failed" a1.log
own=$(sed -n 's/.*Resolved \([^ ]*\) .*/\1/p' a1.log | head -1)
[ -n "$own" ] || fail "could not learn this platform's toolchain" a1.log
"$MCPP" build > a2.log 2>&1 || fail "the second build failed" a2.log
[ "$(resolutions a2.log)" = 0 ] \
    || fail "control: an unchanged second build resolved the toolchain, so the fast path was not taken and nothing below can fail" a2.log

# ── B ───────────────────────────────────────────────────────────────────────
"$MCPP" build --toolchain "$own" > b1.log 2>&1 || fail "--toolchain $own failed" b1.log
[ "$(resolutions b1.log)" != 0 ] \
    || fail "--toolchain $own was answered by the fast path of a build that did not request it" b1.log
MCPP_TOOLCHAIN="$own" "$MCPP" build > b2.log 2>&1 || fail "MCPP_TOOLCHAIN=$own failed" b2.log
[ "$(resolutions b2.log)" = 0 ] \
    || fail "MCPP_TOOLCHAIN=$own is the request --toolchain makes, and it was not answered by that build's fast path" b2.log

# ── A ───────────────────────────────────────────────────────────────────────
"$MCPP" build > a3.log 2>&1 || fail "the build after --toolchain failed" a3.log
[ "$(resolutions a3.log)" != 0 ] \
    || fail "a build without --toolchain was answered by the fast path of the --toolchain build" a3.log
"$MCPP" build > a4.log 2>&1 || fail "the settling build failed" a4.log
[ "$(resolutions a4.log)" = 0 ] || fail "the default request did not return to the fast path" a4.log
echo "build A-B-A OK ($own)"

# ── The same A-B-A through `mcpp run` ─────────────────────────────────────
"$MCPP" run > r1.log 2>&1 || fail "mcpp run failed" r1.log
"$MCPP" run > r2.log 2>&1 || fail "the second mcpp run failed" r2.log
[ "$(resolutions r2.log)" = 0 ] || fail "control: an unchanged second run resolved the toolchain" r2.log
# `mcpp run` takes the request through the environment, the channel
# `--toolchain` itself uses.
MCPP_TOOLCHAIN="$own" "$MCPP" run > r3.log 2>&1 || fail "MCPP_TOOLCHAIN=$own mcpp run failed" r3.log
[ "$(resolutions r3.log)" != 0 ] \
    || fail "MCPP_TOOLCHAIN=$own mcpp run was answered by the fast path of a run that did not request it" r3.log
echo "run A-B-A OK"

# ── The machine default ────────────────────────────────────────────────────
family=${own%@*}
own_version=${own#*@}
export MCPP_HOME="$TMP/home"
source "$HERE/_inherit_toolchain.sh"
other=""
for dir in "$MCPP_HOME/registry/data/xpkgs/xim-x-$family"/*; do
    [ -d "$dir" ] || continue
    v=$(basename "$dir")
    [ "$v" != "$own_version" ] && [ -x "$dir/bin/g++" -o -x "$dir/bin/clang++" -o -x "$dir/bin/clang++.exe" ] \
        && { other="$family@$v"; break; }
done
if [ -z "$other" ]; then
    echo "NOT MEASURED: the machine-default leg, because no second $family version is installed"
    exit 0
fi

rm -rf target .mcpp
"$MCPP" toolchain default "$own" > d0.log 2>&1 || fail "mcpp toolchain default $own failed" d0.log
"$MCPP" build > d1.log 2>&1 || fail "the build with default $own failed" d1.log
"$MCPP" build > d2.log 2>&1 || fail "the second build with default $own failed" d2.log
[ "$(resolutions d2.log)" = 0 ] || fail "control: an unchanged build in the isolated home resolved the toolchain" d2.log

"$MCPP" toolchain default "$other" > d3.log 2>&1 || fail "mcpp toolchain default $other failed" d3.log
"$MCPP" build > d4.log 2>&1 || fail "the build with default $other failed" d4.log
[ "$(resolutions d4.log)" != 0 ] \
    || fail "changing the machine default to $other was answered by the fast path of the $own build" d4.log
grep -q "Resolved $other" d4.log || fail "the build did not resolve the new default $other" d4.log
artefact=$(ls -t $(find target -type f -name 'fastpath*' -path '*/bin/*') | head -1)
if [ "$family" = gcc ] && [ "$(od -An -c -N4 "$artefact" | tr -d ' ')" = '177ELF' ]; then
    grep -aq "${other#*@}" "$artefact" \
        || fail "the artefact the build left does not carry $other's version string" d4.log
fi
echo "machine-default leg OK ($own -> $other)"
