#!/usr/bin/env bash
# requires: llvm qemu-riscv unix-shell
# `mcpp test` on bare metal, and the artifact set a flasher needs.
#
# ⚠️ TWO PLAN ASSUMPTIONS THIS TEST EXISTS BECAUSE THEY WERE WRONG
#
# The design called for a `batch` mode (all cases in one image) and a
# structured stdout protocol, on two premises. Both were measured false:
#
#   * "qemu cold start is ~0.4s, so 30 isolated cases cost 12s"
#     → measured 12ms per start. 30 cases cost 0.36s. The whole reason for
#       batching disappeared, and with it the "on timeout, re-run isolated to
#       find the culprit" machinery — isolated already names the culprit.
#
#   * "bare metal has no exit code to read, so results need their own channel"
#     → semihosting propagates the firmware's `main` return value to the
#       emulator's exit code (`return 7` → qemu exits 7, verified). "Exit code
#       is the verdict" holds here exactly as it does on the host.
#
# So `mcpp test` needed one change — route the test binary through the same
# runner `mcpp run` uses — and this test pins the result of that, from both
# sides: passes pass, and a failure is NAMED and makes the run non-zero.
set -e

QEMU="$(command -v qemu-system-riscv64 || true)"
for d in "$HOME"/.mcpp/registry/data/xpkgs/*-x-qemu-riscv/*/bin \
         "$HOME"/.xlings/data/xpkgs/*-x-qemu-riscv/*/bin; do
    [[ -x "$d/qemu-system-riscv64" ]] && QEMU="$d/qemu-system-riscv64"
done
[[ -n "$QEMU" ]] || { echo "SKIP: no qemu-system-riscv64"; exit 0; }

MH="${MCPP_HOME:-$HOME/.mcpp}"
[[ -d "$MH/registry/data/xpkgs/xim-x-picolibc-riscv" ]] \
    || { echo "SKIP: xim:picolibc-riscv not installed in $MH"; exit 0; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── the board-support package (same shape as 131) ───────────────────────────
"$MCPP" new board > /dev/null
cd board
rm -f src/main.cpp tests/*.cpp 2>/dev/null || true

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
    const char* sysroot = std::getenv("MCPP_XPKG_XIM_PICOLIBC_RISCV_DIR");
    if (!sysroot || !*sysroot) {
        std::cerr << "board: xim:picolibc-riscv declared but not installed\n";
        return 1;
    }
    std::string arch = std::getenv("MCPP_TARGET_ARCH") ?: "";
    std::string prof = (arch == "riscv32") ? "rv32imac/ilp32" : "rv64gc/lp64d";
    std::string rt   = (arch == "riscv32") ? "riscv32" : "riscv64";
    std::string lib  = std::format("{}/lib/{}", sysroot, prof);

    std::println("mcpp:include-dir={}/include/{}", sysroot, prof);
    std::println("mcpp:link-search={}", lib);
    std::println("mcpp:link-lib=crt0-semihost");
    std::println("mcpp:link-lib=c");
    std::println("mcpp:link-lib=semihost");
    std::println("mcpp:link-lib=clang_rt.builtins-{}", rt);
    std::println("mcpp:link-script={}/picolibcpp.ld", lib);

    // ⭐ The runner, from the package that knows the board. The consumer's
    // manifest below has no [target.*] section at all.
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
#include <stdio.h>
export module board;
export namespace board {
    inline void print(const char* s)            { fputs(s, stdout); }
    inline void printf_d(const char* f, int v)  { printf(f, v); }
}
EOF

# ── the consumer: three test cases, one of them failing ─────────────────────
cd "$TMP"
"$MCPP" new fw > /dev/null
cd fw
rm -f tests/*.cpp 2>/dev/null || true

cat > src/main.cpp <<'EOF'
import board;
extern "C" int main() { board::print("firmware\n"); return 0; }
EOF
cat > tests/ok_one.cpp <<'EOF'
import board;
extern "C" int main() { board::print("case one\n"); return 0; }
EOF
cat > tests/ok_two.cpp <<'EOF'
import board;
extern "C" int main() { board::printf_d("case two %d\n", 2); return 0; }
EOF
cat > tests/deliberate_fail.cpp <<'EOF'
import board;
// Non-zero on purpose. Without it, "all green" would be indistinguishable
// from "the verdict is never read" — which is what a bare-metal test harness
// gets wrong by default.
extern "C" int main() { board::print("case three\n"); return 1; }
EOF

# ⚠️ No [target.*] section: the runner comes from the BSP.
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

# ── mcpp test ───────────────────────────────────────────────────────────────
if "$MCPP" test --target riscv64-none-elf > test.log 2>&1; then
    cat test.log
    echo "a failing test case did not make the run fail"
    exit 1
fi
grep -q 'ok_one ... ok'  test.log || { cat test.log; echo "ok_one did not pass"; exit 1; }
grep -q 'ok_two ... ok'  test.log || { cat test.log; echo "ok_two did not pass"; exit 1; }
# ⚠️ The failure has to be NAMED. "2 passed; 1 failed" without a name is a
# harness that tells you to go looking.
grep -q 'deliberate_fail ... FAIL' test.log || {
    cat test.log; echo "the failing case was not named"; exit 1; }

# ── artifact set ────────────────────────────────────────────────────────────
"$MCPP" build --target riscv64-none-elf > build.log 2>&1
elf="$(find target/riscv64-none-elf -name firmware -type f | head -1)"
[[ -n "$elf" ]] || { cat build.log; echo "no firmware"; exit 1; }
[[ -f "$elf.bin" ]] || { echo "no .bin beside the ELF"; exit 1; }
[[ -f "$elf.map" ]] || { echo "no .map beside the ELF"; exit 1; }

# Capacity is the constraint on a bare-metal target, so the number is printed.
grep -q 'Size .*text .*data .*bss' build.log || {
    cat build.log; echo "no size summary"; exit 1; }

# ⚠️ The one that matters: `.bin` is a real edge on the ELF, not a side effect
# of the link command happening to run. Change a source and its CONTENT must
# change — a mtime-only check would pass even if the edge were missing.
before="$(sha256sum "$elf.bin" | cut -d' ' -f1)"
sed -i 's/firmware\\n/firmware2\\n/' src/main.cpp
"$MCPP" build --target riscv64-none-elf > build2.log 2>&1
after="$(sha256sum "$elf.bin" | cut -d' ' -f1)"
[[ "$before" != "$after" ]] || {
    echo ".bin did not change after a source edit — it is not a real ninja edge"
    exit 1; }

# ⚠️ The `.map` is written by a FLAG on the link command, not by its own edge,
# so it is easy to leave undeclared — and then nothing tracks it. Delete it and
# ninja must put it back; without the implicit-output declaration the ELF is
# up to date, ninja has nothing to do, and the map stays gone.
rm -f "$elf.map"
"$MCPP" build --target riscv64-none-elf > build3.log 2>&1
[[ -f "$elf.map" ]] || {
    cat build3.log
    echo ".map did not come back after being deleted — it is not a declared output"
    exit 1; }

echo "PASS: bare-metal mcpp test names its failure, and the artifact set is real"
