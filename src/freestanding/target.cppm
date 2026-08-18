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
    return out;
}

// Every bare-metal target this build of mcpp knows, for `mcpp target list`
// and for diagnostics that want to say what IS available.
inline std::span<const Spec> known() { return kTable; }

} // namespace mcpp::freestanding
