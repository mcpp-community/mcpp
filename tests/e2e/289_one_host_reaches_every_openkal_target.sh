#!/usr/bin/env bash
# requires: llvm unix-shell
# One source, one host, every machine the ecosystem serves.
#
# ⭐⭐ THIS IS CHEAP BECAUSE OF WHAT THE ECOSYSTEM IS, AND THAT IS THE POINT.
#
# A traditional stack needs a macOS runner to test macOS and a Windows runner
# to test Windows, because the target side comes from a payload that only
# exists there. Over openkal it comes from PACKAGES built from source by one
# retargetable clang — so a single Linux machine can produce an artefact for
# every target, and the shape of that artefact is checkable without leaving it.
# Coverage that would cost six runners costs one loop.
#
# ⚠️ WHAT THIS FILE IS FOR IS BREADTH, NOT DEPTH. 286, 287 and 288 each go deep
# on one arrangement — the native stack, the aarch64 cross with its
# outline-atomics helpers, the machine with no operating system. This asserts
# the one property they cannot: that a change to the engine did not silently
# stop serving a target nobody happened to build that day.
#
# ⚠️ A TARGET THAT CANNOT BE BUILT HERE IS SKIPPED WITH ITS REASON, NEVER
# PASSED OVER SILENTLY. A sweep whose failure mode is "produced no output"
# would report success on a run that swept nothing.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# target | extra dependency lines | what `file` must say
#
# The dependency list is per-target because the platform layer is: openkal is
# an interface and each machine has its own implementation of it. That the C
# library and C++ runtime lines are IDENTICAL across every row is the claim the
# ecosystem makes, and it is visible here as repetition rather than stated.
rows='
x86_64-linux-musl|openkal-musl = "0.3.5"\nopenkal-llvm-runtime = "0.1.3"|ELF 64-bit LSB executable, x86-64|statically linked
aarch64-linux-musl|openkal-musl = "0.3.5"\nopenkal-llvm-runtime = "0.1.3"|ELF 64-bit LSB executable, ARM aarch64|statically linked
x86_64-windows-gnu|openkal-musl = "0.3.5"\nopenkal-llvm-runtime = "0.1.3"\nopenkal-windows = "0.1.5"|PE32+|
riscv64-none-elf|openkal = "0.7.0"\nopenkal-opensbi = { version = "0.1.5", features = ["standalone"] }|ELF 64-bit LSB executable, UCB RISC-V|statically linked
'

built=0; skipped=0; failed=0
printf '\n'
while IFS='|' read -r target deps want_fmt want_link; do
    [ -n "$target" ] || continue

    d="$work/$target"; mkdir -p "$d/src"; cd "$d"
    {
        printf '[package]\nname    = "sweep"\nversion = "0.1.0"\n\n'
        printf '[toolchain]\ndefault = "llvm@22.1.8"\n\n'
        # ⚠️ `sysroot = ""` IS HOW A PROJECT SAYS "NO C LIBRARY", AND IT IS NOT
        # IMPLIED BY THE TARGET. `riscv64-none-elf` names a machine with no
        # operating system; whether the program has a C library is a separate
        # statement, and the ecosystem's own bare-metal example makes it. Left
        # out, the spec package's modules are compiled with exceptions enabled
        # and the platform package's `-fno-exceptions` sources then cannot read
        # the BMI:
        #
        #     error: exception handling was enabled in precompiled file
        #            'openkal.stream.pcm' but is currently disabled
        case "$target" in
          *-none-*) printf '[target.%s]\nsysroot = ""\n\n' "$target" ;;
        esac
        printf '[dependencies]\n'
        printf "$deps\n"
    } > mcpp.toml

    # ⚠️ TWO SOURCES, BECAUSE TWO OF THESE TARGETS HAVE NO C LIBRARY TO PRINT
    # WITH. The bare-metal row reaches the platform interface directly; every
    # other row is an ordinary hosted program. Writing one source that works
    # everywhere would mean writing to the lowest common denominator, which is
    # not what a user of a hosted target does.
    case "$target" in
      *-none-*)
        cat > src/main.cpp <<'CPP'
import openkal.stream;
// `extern "C"` with the signature the platform's entry point calls: with
// `-ffreestanding` the name is an ordinary symbol, not a special one.
extern "C" int main(int, char**, char**) {
    const char m[] = "sweep\n";
    kal_stream_write(kal_stdout(), m, sizeof m - 1);
    return 0;
}
CPP
        ;;
      *)
        cat > src/main.cpp <<'CPP'
#include <cstdio>
#include <string>
int main() {
    std::string s = "sweep";
    std::printf("%s\n", s.c_str());
    return s.size() == 5 ? 0 : 1;
}
CPP
        ;;
    esac

    if ! out="$("$MCPP" build --target "$target" 2>&1)"; then
        case "$out" in
          *"not found in the synced index"*|*"install_packages failed"*)
            echo "  SKIP  $target — its packages are not reachable from here"
            skipped=$((skipped+1)); continue ;;
          *"cannot be built on this host"*)
            echo "  SKIP  $target — this host cannot build it"
            skipped=$((skipped+1)); continue ;;
        esac
        echo "  FAIL  $target — the build failed"
        printf '%s\n' "$out" | grep -iE 'error' | head -3 | sed 's/^/          /'
        failed=$((failed+1)); continue
    fi

    bin="$(find target -type f \( -name sweep -o -name sweep.exe \) | head -1)"
    if [ -z "$bin" ]; then
        echo "  FAIL  $target — the build reported success and produced nothing"
        failed=$((failed+1)); continue
    fi

    desc="$(file -b "$bin")"
    ok=1
    case "$desc" in *"$want_fmt"*) ;; *) ok=0 ;; esac
    if [ -n "$want_link" ]; then
        case "$desc" in *"$want_link"*) ;; *) ok=0 ;; esac
    fi
    if [ "$ok" = 1 ]; then
        echo "  ok    $target → $want_fmt${want_link:+, $want_link}"
        built=$((built+1))
    else
        echo "  FAIL  $target — wrong artefact"
        echo "          want: $want_fmt${want_link:+ + $want_link}"
        echo "          got:  $desc"
        failed=$((failed+1))
    fi
done <<EOF
$rows
EOF

printf '\n  built %d, skipped %d, failed %d\n' "$built" "$skipped" "$failed"

[ "$failed" = 0 ] || exit 1
# ⭐ AND A SWEEP THAT SWEPT NOTHING IS NOT A PASS. Every row skipping is the
# signature of a machine that cannot reach the index at all, which is worth
# reporting as a skip of the whole file rather than as success.
if [ "$built" = 0 ]; then
    echo "SKIP: no target could be built here"
    exit 0
fi

echo "OK: one host reached $built of the ecosystem's targets"
