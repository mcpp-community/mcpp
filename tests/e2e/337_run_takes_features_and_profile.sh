#!/usr/bin/env bash
# requires: gcc unix-shell
# `mcpp run` takes `--features` and `--profile`, the axes `build` and `test` do.
#
# WITHOUT THEM `run` COULD ONLY EXECUTE WHATEVER A PREVIOUS `build` LEFT
# BEHIND. There was no spelling of `mcpp run` that ran a release artefact, or
# one built with a feature on — and a board-support package expresses its two
# environments (an emulator, a debug probe) AS features, so
# `mcpp run --features hardware` is precisely the command the device surface was
# designed around. It did not exist.
#
# AND THE FAST PATH HAD TO LEARN ABOUT THEM. It reuses the cached artefact,
# which was built under the previous feature set and profile; taking it here
# would accept the flag and ignore it, which is worse than refusing it.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/app/src"
cd "$work/app"
cat > mcpp.toml <<'TOML'
[package]
name    = "featrun"
version = "0.1.0"

[build]
sources = ["src/main.cpp"]

[features]
loud = { defines = ["LOUD=1"] }
TOML
cat > src/main.cpp <<'CPP'
#include <cstdio>
int main() {
#ifdef LOUD
    std::printf("LOUD\n");
#else
    std::printf("quiet\n");
#endif
#ifdef NDEBUG
    std::printf("release\n");
#else
    std::printf("dev\n");
#endif
    return 0;
}
CPP

want() { case "$1" in *"$2"*) ;; *) echo "FAIL: $3"; echo "$1"; exit 1 ;; esac; }
lack() { case "$1" in *"$2"*) echo "FAIL: $3"; echo "$1"; exit 1 ;; *) ;; esac; }

plain="$("$MCPP" run 2>&1)"
want "$plain" "quiet" "a plain run should not have the feature"
want "$plain" "dev"   "a plain run should be the dev profile"

# THE SECOND RUN IS THE ONE THAT MATTERS: the first populated the build
# cache, so a fast path that ignored --features would now answer "quiet".
loud="$("$MCPP" run --features loud 2>&1)"
want "$loud" "LOUD" "--features was accepted and ignored (the fast path took a stale entry)"
lack "$loud" "quiet" "--features did not take effect"

rel="$("$MCPP" run --release 2>&1)"
want "$rel" "release" "--release was accepted and ignored"

both="$("$MCPP" run --features loud --release 2>&1)"
want "$both" "LOUD"    "--features lost when combined with --release"
want "$both" "release" "--release lost when combined with --features"

# And back: the flags are not sticky. An entry written under one feature set
# must not answer for a run that asks for none.
again="$("$MCPP" run 2>&1)"
want "$again" "quiet" "a plain run inherited the previous --features"
want "$again" "dev"   "a plain run inherited the previous --release"

# ── And the same defect on `mcpp build`, which is where it came from ───────
#
# THIS WAS PRE-EXISTING AND IS THE REASON THE RUN SIDE WAS BROKEN. The
# build cache entry is keyed on (target, profile, cache mode) while the OUTPUT
# DIRECTORY is keyed on a fingerprint that includes the features. So an entry
# written by `mcpp build --features loud` pointed at the loud directory, and the
# next plain `mcpp build` matched it and reported success in 0.00s — serving a
# featured artefact to a request that had no feature on.
#
# Measured before the fix: three builds of one project printed
# `quiet`, `LOUD`, `LOUD`.
# THE ASSERTION IS ON WHAT THE CACHE ENTRY RECORDS, NOT ON WHICH FILE A
# `find` HAPPENS TO RETURN FIRST.
#
# The first version of this block ran `find target -name featrun | head -1`.
# Two output directories exist by then — one per feature set — and which one
# `find` walks first is filesystem order. It passed locally and failed on CI,
# which is the signature of an assertion that depends on something nobody
# chose.
#
# What the fix actually changed is the ENTRY: it now records the feature set its
# artefacts were built with, and both fast paths compare it. Entries are written
# most-recently-used first, so the first `features=` line in the cache belongs
# to the build that just ran — and reading it is exact.
mru_features() { awk -F= '/^features=/{print $2; exit}' target/.build_cache; }

rm -rf target
"$MCPP" build >/dev/null 2>&1
[ -z "$(mru_features)" ] || {
    echo "FAIL: a plain build recorded features '$(mru_features)'"; exit 1; }

"$MCPP" build --features loud >/dev/null 2>&1
[ "$(mru_features)" = "loud" ] || {
    echo "FAIL: --features loud recorded '$(mru_features)', expected loud"; exit 1; }

# THE ONE THAT CAUGHT THE PRE-EXISTING DEFECT. Before the entry carried a
# feature set, this plain build matched the entry `--features loud` had written,
# reported success in 0.00s and left the loud artefact in place. Measured: three
# consecutive builds of one project printed `quiet`, `LOUD`, `LOUD`.
"$MCPP" build >/dev/null 2>&1
[ -z "$(mru_features)" ] || {
    echo "FAIL: a plain build after --features matched an entry recorded as '$(mru_features)'"
    exit 1; }

echo "PASS: mcpp run takes --features and --profile, and the fast path honours both"
