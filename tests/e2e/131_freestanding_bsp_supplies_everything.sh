#!/usr/bin/env bash
# requires: llvm qemu-riscv unix-shell
# The ecosystem shape: a BOARD-SUPPORT package supplies the C library, the
# startup code, the memory layout and the ISA profile, and the consumer's
# manifest says only "depend on it".
#
# What this proves that 130 does not: 130 shows the ENGINE can build and boot a
# freestanding image, but its project still writes the linker script itself and
# calls no libc. That is "mcpp supports bare metal". This is the thing after
# it — a user who never types picolibc, compiler-rt, crt0, `-nostdlib`,
# `-mcmodel` or a load address, and still gets `printf` with a float.
#
# ⚠️ THE SEAM UNDER TEST (measured 2026-08-19, probe Z1)
#
#   link-search / link-lib / link-script   Scope::LinkGlobal   → reach the consumer
#   include-dir / cflag / cfg              Scope::PackagePrivate → do NOT
#
# That asymmetry is deliberate (a build-time program must not silently widen a
# package's public compile interface), and it is why the BSP includes the
# target's libc headers PRIVATELY and exports a C++ module. A consumer that
# could `#include <stdio.h>` would be getting a header for a target its own TU
# may not be built for.
#
# It also depends on `xpkg_dir` — the interface that answers "where did the
# package I declared in [xlings] deps land". Without it the BSP would have to
# reconstruct `<home>/data/xpkgs/<ns>-x-<name>/<version>`, which is store
# internals mcpp is free to change.
set -e


# The sysroot has to be installed in the home MCPP uses, which is not
# necessarily the ambient xlings home. Skip rather than fail: this test is
# about the seam, and a missing payload is an environment fact.
MH="${MCPP_HOME:-$HOME/.mcpp}"
SYSROOT_ROOT="$MH/registry/data/xpkgs/xim-x-picolibc-riscv"
[[ -d "$SYSROOT_ROOT" ]] || { echo "SKIP: xim:picolibc-riscv not installed in $MH"; exit 0; }

# Resolve the emulator to a PATH-independent absolute path.
#
# The runner template may name it bare — that is what a user writes — but this
# test is about mcpp's runner MECHANISM, not about shim/home topology. Measured
# in CI: `qemu-system-riscv64 --version` succeeded in one step while `mcpp run`
# execing the same bare name answered "xlings: 'qemu-system-riscv64' is not
# installed", because the shim on PATH dispatches against its owner home. A
# real BSP has the same information and would emit an absolute path too.
QEMU="$(command -v qemu-system-riscv64 || true)"
for d in "$HOME"/.mcpp/registry/data/xpkgs/*-x-qemu-riscv/*/bin \
         "$HOME"/.xlings/data/xpkgs/*-x-qemu-riscv/*/bin; do
    [[ -x "$d/qemu-system-riscv64" ]] && QEMU="$d/qemu-system-riscv64"
done
[[ -n "$QEMU" ]] || { echo "SKIP: no qemu-system-riscv64"; exit 0; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── the board-support package ───────────────────────────────────────────────
"$MCPP" new board > /dev/null
cd board
rm -f src/main.cpp

cat > mcpp.toml <<'EOF'
[package]
name    = "board"
version = "0.1.0"

[xlings]
deps = ["xim:picolibc-riscv@1.8.12", "xim:qemu-riscv@9.2.4-1"]
EOF

cat > build.mcpp <<'EOF'
import std;
int main() {
    // The interface, not a reconstructed store path.
    const char* sysroot = std::getenv("MCPP_XPKG_XIM_PICOLIBC_RISCV_DIR");
    if (!sysroot || !*sysroot) {
        std::cerr << "board: xim:picolibc-riscv declared but not installed\n";
        return 1;
    }
    // The profile comes from the target mcpp resolved, so the same BSP serves
    // rv32 by reading one variable rather than by being forked.
    std::string arch = std::getenv("MCPP_TARGET_ARCH") ?: "";
    std::string prof = (arch == "riscv32") ? "rv32imac/ilp32" : "rv64gc/lp64d";
    std::string rt   = (arch == "riscv32") ? "riscv32" : "riscv64";
    std::string lib  = std::format("{}/lib/{}", sysroot, prof);

    // PACKAGE-PRIVATE on purpose: these are riscv*-none-elf headers.
    std::println("mcpp:include-dir={}/include/{}", sysroot, prof);
    // LinkGlobal — these reach the consumer's link line.
    std::println("mcpp:link-search={}", lib);
    std::println("mcpp:link-lib=crt0-semihost");
    std::println("mcpp:link-lib=c");
    std::println("mcpp:link-lib=semihost");
    std::println("mcpp:link-lib=clang_rt.builtins-{}", rt);
    std::println("mcpp:link-script={}/picolibcpp.ld", lib);
    // ⭐ The runner too: the package that knows the board resolves the
    // emulator absolutely and says how to drive it. The consumer's manifest
    // below has no [target.*] section at all — that is N1 of the plan.
    const char* qemu = std::getenv("MCPP_XPKG_XIM_QEMU_RISCV_DIR");
    if (qemu && *qemu) {
        std::println("mcpp:runner={}/bin/qemu-system-riscv64", qemu);
        for (auto a : {"-machine","virt","-nographic","-no-reboot",
                       "-semihosting","-bios","none","-kernel"})
            std::println("mcpp:runner={}", a);
    }
    std::println("mcpp:rerun-if-env-changed=MCPP_TARGET_ARCH");
    return 0;
}
EOF

cat > src/board.cppm <<'EOF'
module;
// Private to this package — see the include-dir note in build.mcpp.
#include <stdio.h>
#include <stdlib.h>
export module board;
export namespace board {
    inline void  print(const char* s)             { fputs(s, stdout); }
    inline void  printf_d(const char* f, int v)   { printf(f, v); }
    inline void  printf_f(const char* f, double v){ printf(f, v); }
    inline void* alloc(unsigned long n)           { return malloc(n); }
    inline void  release(void* p)                 { free(p); }
}
EOF

# ── the consumer ────────────────────────────────────────────────────────────
cd "$TMP"
"$MCPP" new fw > /dev/null
cd fw

cat > src/main.cpp <<'EOF'
import board;
extern "C" int main() {
    board::printf_d("BSP-CHAIN-OK %d\n", 42);
    // A FLOAT on purpose: picolibc's ryu formatting does 128-bit shifts, and
    // that is the only ordinary operation needing __ashlti3/__lshrti3 on
    // rv64gc. A 64-bit division would NOT reach it — rv64gc has a hardware
    // divu — so a "does arithmetic work" test is a false green for the
    // builtins the BSP links.
    board::printf_f("float %.4f\n", 3.14159);
    void* p = board::alloc(64);
    board::print(p ? "MALLOC-OK\n" : "MALLOC-FAIL\n");
    board::release(p);
    return 0;
}
EOF

# ⚠️ This manifest IS the assertion. Nothing here names picolibc, compiler-rt,
# crt0, a linker script, a load address, -nostdlib, -mcmodel — or an emulator.
# There is no [target.*] section at all: the runner comes from the BSP.
cat > mcpp.toml <<'EOF'
[package]
name    = "fw"
version = "0.1.0"

[dependencies]
board = { path = "../board" }

[targets.firmware]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" run --target-triple riscv64-none-elf > run.log 2>&1 || true
grep -q 'BSP-CHAIN-OK 42' run.log || {
    cat run.log; echo "the consumer did not run"; exit 1; }
grep -q 'float 3.1416'   run.log || {
    cat run.log; echo "float printf failed — builtins missing from the link?"; exit 1; }
grep -q 'MALLOC-OK'      run.log || {
    cat run.log; echo "the heap is not wired"; exit 1; }

# ── the private half of the seam, pinned from the other side ────────────────
# If include-dir DID reach the consumer, this would compile — and the whole
# reason the BSP exports a module would be gone.
cat > src/main.cpp <<'EOF'
#include <stdio.h>
extern "C" int main() { printf("leaked\n"); return 0; }
EOF
if "$MCPP" build --target riscv64-none-elf > leak.log 2>&1; then
    echo "a dependency's include-dir reached the consumer — it is supposed to be"
    echo "package-private (mcpp.build.directives, Scope::PackagePrivate)"
    exit 1
fi

echo "PASS: BSP supplies the sysroot, linker script and runtime; consumer only depends on it"
