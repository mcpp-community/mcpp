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
import mcpp.toolchain.builtin_targets;
import mcpp.toolchain.target_rows;

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
    // FLAGS THIS ISA REQUIRES THAT THE FOUR COLUMNS ABOVE CANNOT EXPRESS.
    //
    // The table had three flag columns because three were enough for RISC-V and
    // aarch64, where everything a freestanding build needs is an ISA profile, a
    // calling convention and a code model. x86_64 is the first row that needs a
    // fourth thing, and it is not a preference: see `-mno-red-zone` below.
    //
    // Empty for every row that does not, which is every row but one.
    std::span<const std::string_view> extra;

    // NON-EMPTY MEANS "THIS TARGET'S LINK CANNOT GO THROUGH THE COMPILER
    // DRIVER", AND THAT IS A PROPERTY OF CLANG RATHER THAN OF THE ISA.
    //
    // Every freestanding link is driven by clang, which selects a toolchain
    // from the triple and, for the rows that have one, ends up invoking
    // `ld.lld` directly. Measured on llvm 22.1.8, for every spelling of a bare
    // x86_64 triple — `x86_64-none-elf`, `x86_64-unknown-none`, `x86_64-elf`,
    // `x86_64-none-none` — clang instead invokes the HOST'S `g++` as the
    // linker driver:
    //
    //     g++: error: unrecognized command-line option
    //          '-fuse-ld=/…/llvm/22.1.8/bin/ld.lld'
    //
    // clang has a BareMetal toolchain for arm, aarch64 and riscv and none for
    // x86_64, so that triple falls through to the generic GCC toolchain, whose
    // linker IS gcc. No flag changes it: `-fuse-ld=lld`, `--ld-path=`,
    // `--gcc-toolchain=`, `-B` were each measured and each left `g++` in
    // place. Only putting `linux` in the OS position does — and that brings
    // eight host `-L` paths onto a bare-metal link, which is the hermeticity
    // this engine exists to keep.
    //
    // So for such a row the engine links with `ld.lld` itself and this column
    // carries the emulation name that clang would have passed. Empty for the
    // riscv and aarch64 rows: their driver already reaches lld, and changing a
    // working link to prove a point is how a regression gets introduced.
    std::string_view lldEmulation;
};

// THE ROWS ARE DATA (design 2026-11-09; SPEC-016): each bare-metal row of
// modules/toolchain-model/data/builtin.toml states its ISA profile --
// `march`, `mabi`, `code-model`, `multilib`, `flags`, `link-emulation` -- with
// the measurements behind each value beside it. This table is that data in the
// shape the bare-metal code reads; a row with no `march` is not a bare-metal row.
inline constexpr auto kTable = [] {
    constexpr auto& rows = mcpp::toolchain::builtin::kRows;
    constexpr std::size_t n = [] {
        std::size_t c = 0;
        for (auto const& r : mcpp::toolchain::builtin::kRows) c += r.march.empty() ? 0 : 1;
        return c;
    }();
    std::array<Spec, n> out{};
    std::size_t i = 0;
    for (auto const& r : rows) {
        if (r.march.empty()) continue;
        out[i++] = Spec{ r.triple, r.march, r.mabi, r.codeModel, r.multilib, r.flags, r.linkEmulation };
    }
    return out;
}();

// THE MACHINE DESCRIPTION OF A ROW A PLATFORM DEFINES (SPEC-016 §2.1; design
// 2026-11-10 E1, mcpp#911 N7). The engine uses it as written, as it uses a
// built-in row's: the defined `i386-svr4` row's `march = "i386"` and
// `link-emulation = "elf_i386"` were read and then reached no command line,
// because the table above is compiled from the built-in rows alone. A defined
// row lives in the registry for the rest of the process, so the views stay valid.
//
// A ROW MAY STATE ITS LINK WITHOUT ITS MACHINE (2026-11-13): `x86_64-uefi`
// leaves the processor to the compiler's default for the clang triple and
// states `link-emulation = "i386pep"`, the COFF link of ld.lld's MinGW driver.
// Either column makes the description; `row_words` emits only what is stated.
inline std::optional<Spec> defined_spec(std::string_view triple) {
    auto const* d = mcpp::toolchain::target::defined_row(triple);
    if (!d || (d->march.empty() && d->linkEmulation.empty())) return std::nullopt;
    return Spec{ d->triple, d->march, d->mabi, d->codeModel, d->multilib, d->flags, d->linkEmulation };
}

// The single read point. Returns nullopt for anything that is not a known
// bare-metal target — including hosted triples, so a caller can use this as
// "is this a target I know how to build freestanding".
inline std::optional<Spec> resolve(const mcpp::toolchain::triple::Triple& t) {
    if (!t.is_freestanding()) return std::nullopt;
    const std::string s = t.str();
    for (const auto& row : kTable)
        if (row.triple == s) return row;
    return defined_spec(s);
}

// The machine description of a row with an operating system under it: only a
// row a platform defines can state one (every built-in hosted row leaves it to
// the compiler's defaults), and it reaches each compile as the bare-metal row's
// does, and the link when the platform links with `ld.lld` itself.
inline std::optional<Spec> hosted_spec(std::string_view triple) {
    auto t = mcpp::toolchain::triple::parse(triple);
    if (!t || t->is_freestanding()) return std::nullopt;
    return defined_spec(t->str());
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
// `targetCxxRuntime` — a package in the graph supplies a C++ runtime BUILT FOR
// THIS TARGET (libc++abi and an unwinder). The comment below predicted this
// case and named it as the point at which the exception/RTTI pair stops being
// unconditional; the caller answers it from the capability the graph declares,
// so the answer is the graph's rather than a guess about the target.
// THE ROW'S OWN WORDS: its machine description; everything `compile_flags` adds
// after them is the engine's convention (`mcpp why flags` tells them apart).
inline std::vector<std::string> row_words(const Spec& s) {
    std::vector<std::string> out;
    if (!s.march.empty()) out.emplace_back(std::string("-march=") + std::string(s.march));
    if (!s.mabi.empty())
        out.emplace_back(std::string("-mabi=")  + std::string(s.mabi));
    if (!s.mcmodel.empty())
        out.emplace_back(std::string("-mcmodel=") + std::string(s.mcmodel));
    // Whatever the three columns above could not say. Emitted before
    // `-ffreestanding` so the ordering of this function's output stays a
    // function of the table rather than of the row.
    for (auto flag : s.extra) out.emplace_back(flag);
    return out;
}

// The row's own words of a link by ld.lld itself: the emulation, one word.
inline std::vector<std::string> row_link_words(const Spec& s) {
    if (s.lldEmulation.empty()) return {};
    return { "-m " + std::string(s.lldEmulation) };
}

inline std::vector<std::string> compile_flags(const Spec& s,
                                              bool targetCxxRuntime = false,
                                              bool hostedC = false) {
    std::vector<std::string> out = row_words(s);
    // ONE SECTION PER FUNCTION, SO THE LINKER CAN DROP WHAT NOTHING CALLS.
    //
    // These two flags do nothing on their own; they are the half of
    // `--gc-sections` that has to happen at compile time, and the link half is
    // in mcpp.freestanding.linkline. Both halves are here rather than left to a
    // project because a dependency's translation units must carry them too, and
    // a project cannot reach those.
    //
    // WHY THIS BECAME NECESSARY RATHER THAN MERELY NICE. A dependency's
    // object files enter the link unconditionally (docs/13), unlike an archive
    // member, which is pulled only while its symbol is undefined. That costs
    // nothing when the C library is a prebuilt archive and the target has
    // megabytes — the arrangement every bare-metal row had until now. A C
    // library that arrives from the dependency graph is object files, and a
    // Cortex-M part has kilobytes: without this, every image carries the whole
    // of the C library whether or not it calls into it.
    //
    // AND IT MAKES A LINKER SCRIPT LOAD-BEARING IN A NEW WAY: an interrupt
    // vector table is referenced by nothing — the hardware reads it by address
    // — so `--gc-sections` collects it. A board's script must say
    // `KEEP(*(.vectors))`. Measured: with the KEEP present, a dead function is
    // dropped and the table survives; the image boots.
    out.emplace_back("-ffunction-sections");
    out.emplace_back("-fdata-sections");
    // AND `-ffreestanding` ITSELF IS ONE OF THE THINGS THE GRAPH DECIDES.
    //
    // The paragraph above this function names what the flag changes: "no `main`
    // special-casing, no builtin-to-libcall rewrites it cannot back up". Both
    // are statements about whether a library is there — and when a package in
    // the graph provides `hosted-standard-library` FOR THIS TARGET, one is.
    // `hosted` is the language's own word for not-freestanding, so a provider
    // of that capability is asserting exactly the condition this flag denies.
    //
    // Measured 2026-08-23, and the way it showed was not a diagnostic about
    // the flag. A bare-metal program whose `main` was an ordinary C++ `int
    // main()` failed to link with `undefined symbol: main`, while `nm` on its
    // own object showed `_Z4mainv` — under `-ffreestanding` a C++ `main` is not
    // the reserved entry point and is therefore mangled like any other
    // function. The startup object referred to `main` and nothing defined it.
    //
    // The alternative was to make every such program write `extern "C" int
    // main()`, which is a workaround for a claim the build was making on the
    // program's behalf and that was no longer true.
    if (!targetCxxRuntime && !hostedC) out.emplace_back("-ffreestanding");
    // UNWIND TABLES, WHICH THE COMPILER TURNS OFF FOR THIS KIND OF TARGET
    // AND WHICH NOTHING IN THE BUILD OTHERWISE SAYS.
    //
    // On a hosted ELF target clang emits `.eh_frame` for every function by
    // default. On a bare-metal ELF target it does not — the assumption being
    // that nothing will ever unwind. When a C++ runtime IS present for the
    // target that assumption is wrong, and the way it is wrong is specific:
    // the tables appear for anything compiled with `-fexceptions` (libc++abi,
    // libunwind's C++ half, the program) and are ABSENT for everything else,
    // which on this stack means the C library and libunwind's own C sources.
    //
    // AND A PARTIAL SET OF TABLES DOES NOT DEGRADE — IT STOPS THE WALK.
    // Measured 2026-08-23 on riscv64-none-elf, and the measurement is worth
    // keeping because every intermediate reading pointed elsewhere:
    //
    //     __unw_get_proc_info  -> 0  start=80200148 end=8020068c lsda=80447190
    //     __unw_step           -> 0   (UNW_STEP_END)
    //     after step           -> 8021d55e
    //
    // The frame WAS found, its personality data WAS found, the step DID compute
    // a return address — and `step` still reported the end of the stack,
    // because libunwind re-derives the info for the caller and the caller was
    // `__libc_start_main`, a C function with no table. `_Unwind_RaiseException`
    // lives in libunwind's own `UnwindLevel1.c` and has none either, so a throw
    // ends at the first step with `terminating due to uncaught exception`.
    //
    // The asynchronous form rather than `-funwind-tables`: it is what a hosted
    // ELF target already gets by default, and the rest of this stack was
    // developed against that behaviour.
    if (targetCxxRuntime) out.emplace_back("-fasynchronous-unwind-tables");
    // No C++ standard library headers. Not a preference — the toolchain's
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
    // Kept in both cases: a target-side C++ library reaches a consumer through
    // its own include dirs and modules, never through the compiler's.
    out.emplace_back("-nostdinc++");
    // Exceptions and RTTI off, and this belongs HERE — with the target — for
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
    //
    // AND THAT POINT HAS ARRIVED. A package that provides the capability
    // `hosted-standard-library' for this target IS the board described above:
    // it carries libc++abi and libunwind compiled for it. With one present,
    // forcing these off is what breaks the build --- the runtime is compiled
    // with exceptions because it IMPLEMENTS them, and a graph that disagrees
    // with it reports the same `exception handling was enabled in precompiled
    // file' the paragraph above quotes.
    if (!targetCxxRuntime) {
        out.emplace_back("-fno-exceptions");
        out.emplace_back("-fno-rtti");
    }
    return out;
}

// Every bare-metal target this build of mcpp knows, for `mcpp target list`
// and for diagnostics that want to say what IS available.
inline std::span<const Spec> known() { return kTable; }

} // namespace mcpp::freestanding
