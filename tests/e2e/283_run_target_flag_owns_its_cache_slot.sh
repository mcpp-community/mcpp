#!/usr/bin/env bash
# requires: gcc
# `mcpp run --target X` must not write X's build into the HOST's cache slot.
#
# WHAT MAKES THIS FAIL SILENTLY RATHER THAN LOUDLY.
#
# The build cache is keyed on the target triple, and `mcpp run`'s fast path
# matches the entry whose key is EMPTY — empty means "built for this machine",
# which is the only case where exec'ing a cached artifact directly is correct.
# `build_run_target` received the `--target` value, used it to prepare a correct
# cross build, and then did not pass it to the function that writes the cache.
# The cross build was recorded under the host's key.
#
# Nothing about that build is wrong and nothing reports it. The damage lands on
# the NEXT command: a bare `mcpp run` finds an entry claiming to be a host
# build, skips prepare_build entirely — so no host artifact is produced and no
# runner is resolved — and exec's the foreign binary on this machine.
#
# Measured on 2026.8.24.3 with a bare-metal target, from a clean `target/`:
#
#     $ mcpp run --target riscv64-none-elf        # correct, ran under qemu
#     $ mcpp run
#          Running `target/riscv64-none-elf/…/bin/openkal-same-source`
#     exit=1
#
# THE ASSERTION IS ON THE PATH, NOT ON THE EXIT CODE. A foreign-architecture
# artifact fails to exec, so a test checking only the status would pass for the
# wrong reason there and MISS the defect wherever the foreign binary happens to
# run — which is what a same-architecture cross build does. What is wrong is
# which directory the program came from.
#
# The comment on `try_fast_run` records this same defect reached through the
# manifest's `[build] target` and guards that door. This is the other door.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

mkdir -p app/src
cat > app/mcpp.toml <<'TOML'
[package]
name    = "runslot"
version = "0.1.0"
TOML

cat > app/src/main.cpp <<'CPP'
#include <cstdio>
int main() { std::printf("host-artifact\n"); }
CPP

cd app

# A second target this machine can build AND run: the same architecture and OS,
# spelled with the other C library. A bare-metal target would expose the defect
# through a crash, which is the weaker signal — see the note above.
cross=x86_64-linux-musl

if ! "$MCPP" build --target "$cross" >/dev/null 2>&1; then
    echo "SKIP: this machine cannot build $cross"; exit 0
fi

rm -rf target
if ! "$MCPP" run --target "$cross" >/dev/null 2>&1; then
    echo "SKIP: $cross builds but does not run here"; exit 0
fi

# ── The command under test ──────────────────────────────────────────────────
if ! out="$("$MCPP" run 2>&1)"; then
    echo "FAIL: a bare \`mcpp run\` after a cross run exited non-zero"
    printf '%s\n' "$out" | tail -5
    exit 1
fi

# The load-bearing line. `Running `…`` names the artifact's path, and the
# path names the target it was built for.
ran="$(printf '%s\n' "$out" | sed -n 's/.*Running `\([^`]*\)`.*/\1/p' | head -1)"
if [ -z "$ran" ]; then
    echo "FAIL: no \`Running\` line in the output"; printf '%s\n' "$out"; exit 1
fi

case "$ran" in
  *"/$cross/"*)
    echo "FAIL: a bare \`mcpp run\` exec'd the $cross artifact"
    echo "       $ran"
    echo "       The cross build was recorded under the host's cache key."
    exit 1 ;;
esac

if ! printf '%s\n' "$out" | grep -q 'host-artifact'; then
    echo "FAIL: the host program's own output is missing"
    printf '%s\n' "$out"
    exit 1
fi

echo "OK: \`mcpp run --target $cross\` left the host's cache slot alone"
