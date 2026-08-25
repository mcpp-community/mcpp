#!/usr/bin/env bash
# requires: llvm unix-shell
# The whole target side from packages: kernel interface, C library, C++ runtime.
#
# ⚠️ WHY THIS FILE EXISTS, AND WHAT IT COST NOT TO HAVE IT.
#
# mcpp and openkal are separate projects, and the engine names no
# implementation — that separation is the point of `mcpp:<layer>` capabilities.
# It is not a reason for the engine to be untested against the one ecosystem
# that exercises every layer it models, and until this file there were eight
# e2e scripts mentioning openkal of which SEVEN used synthetic manifests: a
# package invented on the spot that claims `provides = ["mcpp:kernel-abi=…"]`.
# Those test what the engine does with a declaration. They cannot test what it
# does with the ecosystem.
#
# Measured 2026-08-25. A change to how the target side is resolved (#486)
# replaced the link line whenever `kernelAbi.fromGraph() || cAbi.fromGraph()`,
# which is right for a graph-supplied C library and wrong for a payload one.
# Every openkal backend broke — that combination is how a backend is tested —
# and mcpp's own CI stayed green through four releases, because no synthetic
# manifest had the shape. See `285_…` for the narrow case; this file covers the
# arrangement the ecosystem actually ships.
#
# ⭐ THE ASSERTIONS ARE ABOUT THE ARTEFACT, NOT ABOUT EXIT CODES. A build that
# resolves the wrong C library still exits 0; what it cannot do is produce a
# static image with no interpreter and no reference to the host's loader.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app/src"
cd "$work/app"

cat > mcpp.toml <<'TOML'
[package]
name    = "okstack"
version = "0.1.0"

# openkal-llvm-runtime IS libc++, libc++abi and libunwind; a build of it with
# gcc is not a thing that exists, and the package says so. Declaring the
# toolchain here rather than relying on a global default keeps this test from
# depending on how the machine running it is configured.
[toolchain]
default = "llvm@22.1.8"

[dependencies]
openkal-musl = "0.3.5"
openkal-llvm-runtime = "0.1.3"
TOML

cat > src/main.cpp <<'CPP'
#include <cstdio>
#include <string>
#include <vector>

// Enough of the standard library to need the C++ runtime, the C library and
// the platform interface at once: a heap allocation, a formatted write, and a
// container that grows.
int main() {
    std::vector<std::string> v;
    for (int i = 0; i < 4; ++i) v.push_back("x" + std::to_string(i));
    std::string joined;
    for (auto const& s : v) joined += s;
    std::printf("%s %zu\n", joined.c_str(), v.size());
    return joined == "x0x1x2x3" && v.size() == 4 ? 0 : 1;
}
CPP

if ! out="$("$MCPP" build 2>&1)"; then
    case "$out" in
      *"not found in the synced index"*|*"install_packages failed"*)
        echo "SKIP: the openkal packages are not reachable from here"
        exit 0 ;;
    esac
    echo "FAIL: the openkal stack did not build"
    printf '%s\n' "$out" | grep -iE 'error' | head -5
    exit 1
fi

# ── Every layer came from the graph, and the report says which ──────────────
#
# Asserted by layer rather than by counting lines: a report that lost a layer
# would still have three of them, and a test that only checked for the word
# `graph` would pass on a build that took its C library from the payload.
rc=0
for layer in kernel-abi c-abi 'c++-abi'; do
    case "$out" in
      *"$layer"*graph*) echo "  ok  $layer from the graph" ;;
      *) echo "FAIL: $layer did not come from the graph"
         printf '%s\n' "$out" | grep -E 'kernel-abi|c-abi|c\+\+-abi' | sed 's/^/        /'
         rc=1 ;;
    esac
done
[ "$rc" = 0 ] || exit 1

bin="$(find target -type f -name okstack | head -1)"
[ -n "$bin" ] || { echo "FAIL: no artefact"; exit 1; }

# ── The artefact is what a graph-supplied target side produces ──────────────
desc="$(file -b "$bin")"
case "$desc" in
  *"statically linked"*) echo "  ok  statically linked" ;;
  *) echo "FAIL: not static — the payload's C library was linked instead"
     echo "        $desc"; exit 1 ;;
esac

# ⭐ NO INTERPRETER. `statically linked` from `file` is a summary; the program
# header is the fact. A dynamic image names the host's loader here, and that is
# a path the target machine has no reason to have.
if command -v readelf > /dev/null 2>&1; then
    phdrs="$(readelf -l "$bin" 2>/dev/null || true)"
    # ⚠️ THIS ASSERTS AN ABSENCE, SO IT MUST FIRST ESTABLISH THAT SOMETHING WAS
    # READ. `readelf -l` on a file it cannot parse prints zero lines and exits
    # quietly; `grep -q INTERP` then finds nothing, which reads exactly like a
    # static image. e2e 287 shipped that mistake with a disassembler and CI
    # caught it — the same shape, one file over.
    segs="$(printf '%s\n' "$phdrs" \
            | grep -cE '^\s+(LOAD|PHDR|NOTE|GNU_|INTERP|DYNAMIC|TLS)' || true)"
    if [ "${segs:-0}" = 0 ]; then
        echo "FAIL: readelf reported no program headers at all — it did not read $bin"
        exit 1
    fi
    if printf '%s\n' "$phdrs" | grep -q 'INTERP'; then
        echo "FAIL: the image names an interpreter"
        printf '%s\n' "$phdrs" | grep -A1 INTERP | sed 's/^/        /'
        exit 1
    fi
    echo "  ok  no INTERP among $segs program headers — nothing for a loader to resolve"
fi

# ── And it runs, which is the only check the others cannot fake ─────────────
if out="$("$bin" 2>&1)"; then
    case "$out" in
      "x0x1x2x3 4") echo "  ok  it runs and prints the right thing: $out" ;;
      *) echo "FAIL: wrong output: $out"; exit 1 ;;
    esac
else
    echo "FAIL: the artefact does not run: $out"
    exit 1
fi

echo "OK: the openkal stack builds, links statically and runs"
