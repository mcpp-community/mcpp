#!/usr/bin/env bash
# requires: gcc unix-shell
# `mcpp run` takes `--features` and `--profile`, the axes `build` and `test` do.
#
# ⚠️⚠️ WITHOUT THEM `run` COULD ONLY EXECUTE WHATEVER A PREVIOUS `build` LEFT
# BEHIND. There was no spelling of `mcpp run` that ran a release artefact, or
# one built with a feature on — and a board-support package expresses its two
# environments (an emulator, a debug probe) AS features, so
# `mcpp run --features hardware` is precisely the command the device surface was
# designed around. It did not exist.
#
# ⚠️ AND THE FAST PATH HAD TO LEARN ABOUT THEM. It reuses the cached artefact,
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

# ⚠️ THE SECOND RUN IS THE ONE THAT MATTERS: the first populated the build
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
# ⚠️⚠️ THIS WAS PRE-EXISTING AND IS THE REASON THE RUN SIDE WAS BROKEN. The
# build cache entry is keyed on (target, profile, cache mode) while the OUTPUT
# DIRECTORY is keyed on a fingerprint that includes the features. So an entry
# written by `mcpp build --features loud` pointed at the loud directory, and the
# next plain `mcpp build` matched it and reported success in 0.00s — serving a
# featured artefact to a request that had no feature on.
#
# Measured before the fix: three builds of one project printed
# `quiet`, `LOUD`, `LOUD`.
artifact() { find target -type f -name featrun -newermt '-1 day' | head -1; }

rm -rf target
"$MCPP" build >/dev/null 2>&1
first="$(./"$(artifact)")"
"$MCPP" build --features loud >/dev/null 2>&1
"$MCPP" build >/dev/null 2>&1
third="$(./"$(artifact)")"
case "$first" in *quiet*) ;; *) echo "FAIL: the first build was not plain"; exit 1 ;; esac
case "$third" in
    *quiet*) ;;
    *) echo "FAIL: a plain build after --features served the featured artefact"
       echo "      first=$first third=$third"; exit 1 ;;
esac

echo "PASS: mcpp run takes --features and --profile, and the fast path honours both"
