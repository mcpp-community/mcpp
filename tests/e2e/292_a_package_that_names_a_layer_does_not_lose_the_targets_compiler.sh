#!/usr/bin/env bash
# requires: llvm unix-shell
# Declaring which layer a package supplies must not change which compiler can
# emit the target.
#
# MEASURED 2026-08-25, ON A THREE-LINE MANIFEST. Adding one `provides` line
# to a project moved a bare-metal RISC-V build onto the host's g++:
#
#     provides = ["mcpp:kernel-abi=openkal"]
#     $ mcpp build --target riscv64-none-elf
#       Resolved gcc@16.1.0 → riscv64-none-elf → …/bin/g++
#       g++: error: unrecognized argument in option '-mabi=lp64d'
#       g++: error: unrecognized command-line option '--target=riscv64-none-elf'
#
# The cause is a predicate spanning kernel-abi ∪ c-abi that cancelled the target
# row's compiler. For a HOSTED row that is right — the row names the payload
# supplying the target's C library, and a graph that supplies one instead makes
# it inapplicable. A bare-metal row names the only compiler that emits the
# target at all: `clang`/`lld` are cross-compilers by construction and a host
# g++ cannot produce riscv64-none-elf whatever the graph contains.
#
# BOTH DIRECTIONS, BECAUSE ONLY ONE OF THEM IS THE FIX. Never cancelling
# the pin also stops this failing, and it would restore the substitution the
# predicate was added to prevent — a hosted project whose C library comes from
# its graph having its chosen toolchain silently replaced. The second half
# builds that arrangement and requires the choice to survive.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

resolved() {   # dir target → "family@version", or nothing
    (cd "$1" && "$MCPP" build --target "$2" 2>&1 || true) \
        | grep -oP 'Resolved \K[a-z]+@[0-9.]+' | head -1
}

make_project() {   # dir provides-line
    mkdir -p "$1/src"
    { printf '[package]\nname    = "layerprobe"\nversion = "0.1.0"\n'
      [ -n "$2" ] && printf '%s\n' "$2"; } > "$1/mcpp.toml"
    printf 'extern "C" void _start() { for (;;) {} }\n' > "$1/src/main.cpp"
}

# ── Half one: a bare-metal target keeps the compiler that can emit it ──────
#
# The control comes first: without the declaration the row's pin is what any
# machine resolves, and a test that never established that baseline could not
# tell "the fix works" from "this machine has no llvm".
make_project "$work/plain" ""
base="$(resolved "$work/plain" riscv64-none-elf)"
case "$base" in
  llvm@*) echo "  ok  baseline: the bare-metal row resolves $base" ;;
  *) echo "SKIP: the bare-metal row did not resolve to llvm here (got '${base:-nothing}')"
     exit 0 ;;
esac

make_project "$work/declares" 'provides = ["mcpp:kernel-abi=openkal"]'
after="$(resolved "$work/declares" riscv64-none-elf)"
if [ "$after" = "$base" ]; then
    echo "  ok  and it still resolves $after after the package names a layer"
else
    echo "FAIL: naming a layer changed the target's compiler"
    echo "        without provides: $base"
    echo "        with provides:    ${after:-nothing}"
    exit 1
fi

# ── Half two: a hosted project's own choice still wins over the row ────────
#
# `x86_64-linux-musl` carries `gcc@16.1.0` in the table because the musl-gcc
# payload is what supplies that target's C library. A project whose C library
# comes from its graph does not use that payload, so the row must not replace
# a toolchain the user set.
mkdir -p "$work/hosted/src"
cat > "$work/hosted/mcpp.toml" <<'TOML'
[package]
name     = "hostedprobe"
version  = "0.1.0"
provides = ["mcpp:c-abi=musl"]

[toolchain]
default = "llvm@22.1.8"
TOML
printf 'int main() { return 0; }\n' > "$work/hosted/src/main.cpp"
hosted="$(resolved "$work/hosted" x86_64-linux-musl)"
case "$hosted" in
  llvm@*)
    echo "  ok  a hosted row does not replace the project's own toolchain: $hosted" ;;
  "")
    echo "SKIP: the hosted project did not report a resolution here" ;;
  *)
    echo "FAIL: the target row replaced the toolchain this project chose"
    echo "        chose llvm@22.1.8, resolved $hosted"
    exit 1 ;;
esac

echo "OK: naming a layer changes the system, not the compiler that emits the target"
