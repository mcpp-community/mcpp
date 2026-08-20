// mcpp.freestanding.target — what a bare-metal target IS, in one table.
//
// WHY THIS IS ITS OWN MODULE (AND ITS OWN DIRECTORY)
//
// A freestanding target differs from a hosted one along more axes than the
// triple carries: an ISA profile (`-march`), a calling convention (`-mabi`), a
// code model, and whether the C++ runtime pieces (exceptions, RTTI, threads)
// have anything under them. Every one of those is a decision that MUST be
// derived in exactly one place — this codebase has paid repeatedly for the
// opposite shape (#233/#240/#242/#344: "the same decision derived in N places
// does not fail when you add it, it fails later, somewhere else").
//
// So: one row per target, and `resolve()` is the only way to read it. A
// consumer that wants to know "what -march does riscv64-none-elf use" asks
// here; it does not pattern-match the triple.
//
// WHAT IS DELIBERATELY NOT HERE
//
// * No libc. No linker script. No memory map. No emulator arguments. Those
//   belong to a BSP package, and the engine never learns them — the whole
//   point of the layering (see .agents/docs/2026-08-19-baremetal-ecosystem-
//   closure-plan.md §2). This table stops at the ISA.
// * No sysroot. Measured 2026-08-19 (probe Z1): a package's `build.mcpp` can
//   already put `-L`/`-l` on the consumer's link line (`link-search` and
//   `link-lib` are Scope::LinkGlobal), while `include-dir` is PackagePrivate
//   by design. So target headers reach a consumer the modular way — the libc
//   wrapper package includes them privately and exports a C++ module — and the
//   engine needs no sysroot concept at all.

export module mcpp.freestanding.target;

import std;
import mcpp.toolchain.triple;

export namespace mcpp::freestanding {

// One bare-metal target's ISA-level facts.
struct Spec {
    std::string_view triple;      // canonical, e.g. "riscv64-none-elf"
    std::string_view march;       // ISA profile   -march=
    std::string_view mabi;        // calling conv  -mabi=
    std::string_view mcmodel;     // code model    -mcmodel=   ("" = driver default)
    // Sub-directory a libc/sysroot package uses for this profile. picolibc's
    // own multilib convention (`<march>/<mabi>`), carried here so a wrapper
    // package can name its layout without re-deriving the profile.
    std::string_view libdir;
};

// ⚠️ Defaults, not the only possibility. `rv64gc/lp64d` is what qemu `virt`
// runs and what the first BSP targets; a board that needs `rv32imac/ilp32`
// selects it through its own manifest, not by editing this table. The table
// exists so that `--target riscv64-none-elf` alone is enough to produce a
// correct object file — not to enumerate every board.
inline constexpr Spec kTable[] = {
    //  triple                march       mabi     mcmodel    libdir
    { "riscv64-none-elf",   "rv64gc",   "lp64d", "medany", "rv64gc/lp64d"   },
    { "riscv32-none-elf",   "rv32imac", "ilp32", "medany", "rv32imac/ilp32" },
    // ⚠️ `aapcs` AND NOT `lp64`. The two RISC-V rows above spell their ABI the
    // way RISC-V does, and the obvious extrapolation to aarch64 is `lp64` —
    // which is what LP64 aarch64 actually is, and which clang rejects:
    //
    //     error: unknown target ABI 'lp64'
    //
    // aarch64's `-mabi` names a procedure call standard rather than a data
    // model, and the only value it takes for this target is `aapcs`. Measured
    // rather than reasoned: `-mabi=lp64` compiles on riscv64 and fails here,
    // so a table filled in by analogy would have been wrong in exactly the
    // place the analogy is strongest.
    //
    // `small` is the code model: it addresses ±4GiB PC-relative, which covers
    // any single bare-metal image regardless of where the linker script places
    // it. RISC-V needs `medany` for the corresponding reason — its default
    // assumes the low 2GiB, and bare-metal RISC-V runs at 0x80000000.
    //
    // ⚠️ THE LIBDIR COLUMN IS EMPTY, AND THAT IS THE ROW'S CHARACTER. It names
    // a C library's multilib directory, and this target's row in the target
    // table resolves no C library: `aarch64-none-elf` is the zero-libc tier.
    // The engine reads this column only when a sysroot exists, so an invented
    // value would be a value nothing could ever check.
    { "aarch64-none-elf",   "armv8-a",  "aapcs", "small",  ""               },
};

// The single read point. Returns nullopt for anything that is not a known
// bare-metal target — including hosted triples, so a caller can use this as
// "is this a target I know how to build freestanding".
inline std::optional<Spec> resolve(const mcpp::toolchain::triple::Triple& t) {
    if (!t.is_freestanding()) return std::nullopt;
    const std::string s = t.str();
    for (const auto& row : kTable)
        if (row.triple == s) return row;
    return std::nullopt;
}

inline std::optional<Spec> resolve(std::string_view triple) {
    auto t = mcpp::toolchain::triple::parse(triple);
    if (!t) return std::nullopt;
    return resolve(*t);
}

// Compile flags this target needs on EVERY translation unit, freestanding or
// not. Emitted from the spec rather than hardcoded per call site.
//
// `-ffreestanding` is here and not in the link layer on purpose: it changes
// what the compiler may assume about the library (no `main` special-casing, no
// builtin-to-libcall rewrites it cannot back up), and that assumption has to
// hold for every TU in the build, including a dependency's.
inline std::vector<std::string> compile_flags(const Spec& s) {
    std::vector<std::string> out;
    out.emplace_back(std::string("-march=") + std::string(s.march));
    out.emplace_back(std::string("-mabi=")  + std::string(s.mabi));
    if (!s.mcmodel.empty())
        out.emplace_back(std::string("-mcmodel=") + std::string(s.mcmodel));
    out.emplace_back("-ffreestanding");
    // ⚠️ No C++ standard library headers. Not a preference — the toolchain's
    // libc++ headers are built for the HOST: `#include <stdio.h>` resolves to
    // libc++'s wrapper, which opens `<__config_site>`, which is generated per
    // installation for the host configuration and is simply absent for this
    // target. The error reads as a broken payload
    // (`'__config_site' file not found`) and says nothing about the target.
    //
    // A target-side C++ library is an ordinary package, and the way it reaches
    // a consumer is a MODULE, not an include path: `include-dir` is
    // package-private by design (the supply-chain rule in
    // mcpp.build.directives), so a libc wrapper includes the target headers
    // privately and exports what it wants seen.
    out.emplace_back("-nostdinc++");
    // ⚠️ Exceptions and RTTI off, and this belongs HERE — with the target — for
    // the same reason `-ffreestanding` does: it is a property every TU in the
    // graph must agree on.
    //
    // manifest/types.cppm's `is_dialect_flag` deliberately does NOT propagate
    // `-fno-exceptions` graph-wide, on the grounds that "dependencies may
    // assume exceptions are available". That is right for a hosted target and
    // exactly backwards here: a freestanding target has no unwinder and no
    // libc++abi, so nothing CAN throw — `optional::value()` alone drags in
    // `__cxa_throw`, `vtable for std::exception` and two more, and the link
    // fails with no hint that exceptions were the cause.
    //
    // Leaving it to the project's own `cxxflags` does not work, and the way it
    // fails is worse than not linking. Measured: those flags reach the root's
    // TUs but not a dependency's module compile, so the BMI and the importer
    // disagree and clang says
    //
    //     error: exception handling was enabled in precompiled file
    //     'mcpplibs.riscv_virt_rt.pcm' but is currently disabled
    //
    // which names a module file rather than the setting that split the graph.
    //
    // Not a preference, then, but not permanent either: a board that ships a
    // target-built libc++abi and unwinder has a real case for turning these
    // back on, and that is the point at which this becomes a manifest key.
    out.emplace_back("-fno-exceptions");
    out.emplace_back("-fno-rtti");
    return out;
}

// Every bare-metal target this build of mcpp knows, for `mcpp target list`
// and for diagnostics that want to say what IS available.
inline std::span<const Spec> known() { return kTable; }

} // namespace mcpp::freestanding
