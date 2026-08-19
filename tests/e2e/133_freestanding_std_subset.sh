#!/usr/bin/env bash
# requires: llvm qemu-riscv unix-shell
# The freestanding subset of the standard library, as an ordinary package.
#
# `import std;` is one module over the whole library — threads, filesystem and
# iostreams included — so there is no subset of IT to build without an OS. But
# libc++'s HEADERS are almost entirely freestanding-capable already. What stops
# them is one per-target file, `__config_site`, which the llvm payload ships
# only for its own host triple.
#
# ⚠️ MEASURED, and the control group is what makes the number mean anything:
# with a synthesised `__config_site`, 103 of libc++'s 110 headers compile for
# riscv64-none-elf. The 7 that fail (generator, hazard_pointer, rcu,
# spanstream, stacktrace, stdfloat, text_encoding) fail on an x86_64 host with
# full libc++ and glibc too — they are headers libc++ has not implemented. The
# freestanding loss at compile time is zero. Without the host control group,
# "libc++ has not written it" reads as "bare metal cannot do it".
#
# This test does not re-run that sweep (110 compiles is a package-CI job). It
# pins the MECHANISM the sweep depends on, end to end, in the emulator.
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

# libc++'s headers come from the llvm payload; pick by CONTENT, not by name
# order — an empty directory for an older version sorts first.
LLVM=""
for d in "$MH"/registry/data/xpkgs/xim-x-llvm/*/; do
    [[ -x "$d/bin/clang++" ]] && LLVM="$d"
done
[[ -n "$LLVM" ]] || { echo "SKIP: no llvm payload with clang++"; exit 0; }
LLVM="${LLVM%/}"
[[ -d "$LLVM/share/libc++/v1/std" ]] \
    || { echo "SKIP: llvm payload has no libc++ std/*.inc"; exit 0; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── the board (same shape as 131) ───────────────────────────────────────────
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
    if (!sysroot || !*sysroot) { std::cerr << "no picolibc\n"; return 1; }
    std::string lib = std::format("{}/lib/rv64gc/lp64d", sysroot);
    std::println("mcpp:include-dir={}/include/rv64gc/lp64d", sysroot);
    std::println("mcpp:link-search={}", lib);
    std::println("mcpp:link-lib=crt0-semihost");
    std::println("mcpp:link-lib=c");
    std::println("mcpp:link-lib=semihost");
    std::println("mcpp:link-lib=clang_rt.builtins-riscv64");
    std::println("mcpp:link-script={}/picolibcpp.ld", lib);
    if (const char* q = std::getenv("MCPP_XPKG_XIM_QEMU_RISCV_DIR"); q && *q) {
        std::println("mcpp:runner={}/bin/qemu-system-riscv64", q);
        for (auto a : {"-machine","virt","-nographic","-no-reboot",
                       "-semihosting","-bios","none","-kernel"})
            std::println("mcpp:runner={}", a);
    }
    return 0;
}
EOF
cat > src/board.cppm <<'EOF'
module;
#include <stdio.h>
export module board;
export namespace board {
    inline void print(const char* s)           { fputs(s, stdout); }
    inline void printf_d(const char* f, int v) { printf(f, v); }
}
EOF

# ── the std subset package ──────────────────────────────────────────────────
cd "$TMP"
"$MCPP" new stdfs > /dev/null
cd stdfs
rm -f src/main.cpp tests/*.cpp src/stdfs.cppm 2>/dev/null || true
cat > mcpp.toml <<'EOF'
[package]
name    = "stdfs"
version = "0.1.0"

[xlings]
deps = ["xim:picolibc-riscv@1.8.12", "xim:llvm"]
EOF

# ⚠️ The config has to be a FILE. `_LIBCPP_HAS_THREADS` and friends are read
# from `__config_site` by <__config>; defining them with -D does not reach the
# check at all (measured — the obvious `-D_LIBCPP_HAS_THREADS=0` does nothing).
cat > build.mcpp <<'EOF'
import std;
int main() {
    const char* sysroot = std::getenv("MCPP_XPKG_XIM_PICOLIBC_RISCV_DIR");
    const char* llvm    = std::getenv("MCPP_XPKG_XIM_LLVM_DIR");
    if (!sysroot || !*sysroot) { std::cerr << "no picolibc\n"; return 1; }
    if (!llvm || !*llvm)       { std::cerr << "no llvm payload\n"; return 1; }

    const std::string out = std::getenv("MCPP_OUT_DIR") ?: ".";
    const std::string cfg = out + "/libcxx-config/c++/v1";
    std::filesystem::create_directories(cfg);
    {
        std::ofstream f(cfg + "/__config_site");
        f << "#ifndef _LIBCPP___CONFIG_SITE\n#define _LIBCPP___CONFIG_SITE\n"
             "#define _LIBCPP_ABI_VERSION 1\n#define _LIBCPP_ABI_NAMESPACE __1\n"
             "#define _LIBCPP_ABI_FORCE_ITANIUM 0\n#define _LIBCPP_ABI_FORCE_MICROSOFT 0\n"
             "#define _LIBCPP_HAS_THREADS 0\n#define _LIBCPP_HAS_MONOTONIC_CLOCK 0\n"
             "#define _LIBCPP_HAS_FILESYSTEM 0\n#define _LIBCPP_HAS_LOCALIZATION 0\n"
             "#define _LIBCPP_HAS_TERMINAL 0\n#define _LIBCPP_HAS_RANDOM_DEVICE 0\n"
             "#define _LIBCPP_HAS_TIME_ZONE_DATABASE 0\n#define _LIBCPP_HAS_WIDE_CHARACTERS 0\n"
             "#define _LIBCPP_HAS_UNICODE 1\n#define _LIBCPP_HAS_MUSL_LIBC 0\n"
             "#define _LIBCPP_HAS_THREAD_API_PTHREAD 0\n#define _LIBCPP_HAS_THREAD_API_EXTERNAL 0\n"
             "#define _LIBCPP_HAS_THREAD_API_WIN32 0\n#define _LIBCPP_HAS_THREAD_API_C11 0\n"
             "#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0\n"
             "#define _LIBCPP_INSTRUMENTED_WITH_ASAN 0\n"
             "#define _LIBCPP_PSTL_BACKEND_SERIAL\n"
             "#define _LIBCPP_HARDENING_MODE_DEFAULT 2\n"
             "#define _LIBCPP_ASSERTION_SEMANTIC_DEFAULT 2\n"
             "#define _LIBCPP_LIBC_PICOLIBC 1\n#define _LIBCPP_LIBC_NEWLIB 0\n#endif\n";
    }
    // Order matters: the synthesised config first, libc++ before the C library
    // (<stdio.h> exists in both trees and the C++ wrapper has to win).
    std::println("mcpp:include-dir={}", cfg);
    std::println("mcpp:include-dir={}/include/c++/v1", llvm);
    std::println("mcpp:include-dir={}/share/libc++/v1", llvm);
    std::println("mcpp:include-dir={}/include/rv64gc/lp64d", sysroot);
    return 0;
}
EOF

# The export table is libc++'s, not ours: one `std/<header>.inc` per header,
# each `export namespace std { using std::X; }`. Selecting is the whole job.
cat > src/stdfs.cppm <<'EOF'
module;
#include <array>
#include <span>
#include <optional>
#include <atomic>
#include <string_view>
#include <algorithm>
#include <ranges>
export module mcpplibs.std.freestanding;
#include "std/array.inc"
#include "std/span.inc"
#include "std/optional.inc"
#include "std/atomic.inc"
#include "std/string_view.inc"
#include "std/algorithm.inc"
#include "std/ranges.inc"
EOF

# ⚠️ Declared through libc++'s OWN header. The real symbol lives in the ABI
# inline namespace (std::__1::), so a hand-written `namespace std { ... }`
# definition compiles, links nothing, and leaves the undefined-symbol error
# looking exactly as it did before.
cat > src/verbose_abort.cpp <<'EOF'
#include <__verbose_abort>
_LIBCPP_BEGIN_NAMESPACE_STD
[[noreturn]] void __libcpp_verbose_abort(const char*, ...) _NOEXCEPT {
    for (;;) {
#if defined(__riscv)
        __asm__ volatile("wfi");
#endif
    }
}
_LIBCPP_END_NAMESPACE_STD
EOF

# ── the consumer ────────────────────────────────────────────────────────────
cd "$TMP"
"$MCPP" new app > /dev/null
cd app
rm -f tests/*.cpp 2>/dev/null || true
cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[build]
target = "riscv64-none-elf"

[dependencies]
board = { path = "../board" }
stdfs = { path = "../stdfs" }
EOF
cat > src/main.cpp <<'EOF'
import board;
import mcpplibs.std.freestanding;

struct Task { int prio; const char* name; };

extern "C" int main() {
    // ranges::sort with a PROJECTION: entirely header-resident, unlike the
    // scalar std::sort, whose instantiation libc++ keeps in the compiled
    // library as an extern template.
    std::array<Task, 4> t{{ {3,"c"}, {1,"a"}, {4,"d"}, {2,"b"} }};
    std::ranges::sort(t, {}, &Task::prio);
    for (const auto& x : t) board::print(x.name);
    board::print("\n");

    std::optional<int> o = 41;
    std::atomic<int> a{0};
    a.fetch_add(o.value() + 1);
    board::printf_d("atomic %d\n", a.load());

    std::span<Task> s{t};
    std::string_view sv{"sv"};
    board::printf_d("span %d\n", (int)(s.size() + sv.size()));
    return 0;
}
EOF

"$MCPP" run > run.log 2>&1 || { cat run.log; echo "the std subset did not run"; exit 1; }

grep -q 'abcd'      run.log || { cat run.log; echo "ranges::sort with a projection did not work"; exit 1; }
grep -q 'atomic 42' run.log || { cat run.log; echo "optional/atomic did not work"; exit 1; }
grep -q 'span 6'    run.log || { cat run.log; echo "span/string_view did not work"; exit 1; }

# ── the half that must NOT be there ─────────────────────────────────────────
# Turning a capability off in __config_site makes it VANISH rather than leaving
# a stub that fails at run time. That is the property worth pinning: a
# bare-metal author finds out at compile time, by name.
cat > src/main.cpp <<'EOF'
import board;
import mcpplibs.std.freestanding;
extern "C" int main() { std::mutex m; (void)m; return 0; }
EOF
if "$MCPP" build > mutex.log 2>&1; then
    cat mutex.log
    echo "std::mutex compiled on a target with no threads"
    exit 1
fi
grep -qi "mutex" mutex.log || {
    cat mutex.log; echo "the error did not name std::mutex"; exit 1; }

echo "PASS: the freestanding std subset builds, runs, and omits what it must"
