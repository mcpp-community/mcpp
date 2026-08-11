// mcpp.build.link_line — the ORDER of one link command line, as a declaration.
//
// WHY THIS MODULE EXISTS
//
// DT_RPATH is a SEARCH ORDER, and on this ecosystem two directories routinely
// hold the same SONAME: mcpp compiles `compat.x11` from source into the
// artifact's own directory while xlings has `xim:libX11` in the SubOS library
// view. The loader takes the FIRST match, so the order does not decorate the
// artifact — it SELECTS which physical file the process runs against.
//
// Before this module that order was an accident of three `+=` in two files
// that did not know about each other:
//
//   flags.cppm        appended the SubOS farm to the GLOBAL ldflags,
//                     with a comment saying "so it is LAST"
//   plan.cppm         appended `$ORIGIN` to the PER-UNIT flags
//   ninja_backend     rendered `$cxx $in -o $out $ldflags $unit_ldflags`
//
// Each was locally right and the composition was wrong: the farm landed BEFORE
// `$ORIGIN`, so a GLFW/imgui application linked against the libX11 mcpp had
// just built and then LOADED the one xlings had installed. They are not
// interchangeable, and the program died before main with
// `undefined symbol: _ZNKSt13runtime_error4whatEv`.
//
// The fix is not "append it somewhere else" — that would leave the next caller
// free to make the same mistake. It is to give the line NAMED SLOTS whose
// relative order is written down once, asserted by a unit test, and impossible
// to bypass: a new producer must choose a slot, and choosing is where the
// question "before or after the artifact's own directory?" gets asked.
//
// FORMAT NEUTRALITY. Slots are named by ROLE, never by flag spelling, so no
// consumer needs a platform branch. PE leaves `runtimeFallback` and
// `loaderTag` empty because it has neither; Mach-O puts `@loader_path` in
// `dependencies` where ELF puts `$ORIGIN`. The spellings stay with the
// producers that already know the target format (`plan.cppm` for dependency
// rpath, `distribution.cppm` for the C++ runtime mechanism).
//
// Analysis: .agents/docs/2026-08-11-runtime-search-origin-precedence-analysis.md

export module mcpp.build.link_line;

import std;

export namespace mcpp::build::link_line {

// The per-unit tail of a link command, in emission order.
//
// Everything here follows the GLOBAL flags (toolchain payload `-L`/`-rpath`,
// `--sysroot`, `-B`, `-specs`) and, for a shared library, the soname flag.
// Those are properties of the toolchain and are the same for every unit; this
// struct is what differs per link unit, which is also why the C++ runtime
// mechanism lives here (two roles in one build may hold different contracts).
struct UnitTail {
    // 1. What this unit links AGAINST: dependency `-L`/`-l`, plus the
    //    artifact-relative run-time search path that finds those dependencies
    //    again after the build directory moves (`$ORIGIN` on ELF,
    //    `@loader_path` on Mach-O, nothing on PE — DLLs resolve via the
    //    executable's directory and PATH).
    //
    //    FIRST, and this is the load-bearing decision of the whole module:
    //    these directories hold the EXACT files this link resolved against.
    //    Anything that can supply the same SONAME must come after them, or the
    //    artifact runs against a different build than it was linked against.
    std::string dependencies;

    // 2. The C++ runtime mechanism for this unit's role — `-static-libstdc++`,
    //    Mach-O's `-load_hidden` archives, MinGW's `-static`, and the
    //    `--exclude-libs` guard that keeps a statically embedded standard
    //    library out of a shared object's dynamic symbol table.
    //
    //    After (1) so that a dependency's own definition is found first: an
    //    archive member is pulled only for symbols still undefined at the
    //    point the archive is processed, which is the ordering C++ drivers
    //    have always assumed.
    std::string cxxRuntime;

    // 3. LAST-RESORT run-time search — today the SubOS library view ("the
    //    farm"), a flat symlink tree that `xlings install` rewrites.
    //
    //    It may only supply what nothing else does. It must therefore follow
    //    BOTH the global payload directories AND slot (1): a mutable view that
    //    outranks either of them lets a later install silently change which
    //    library an ALREADY LINKED artifact loads. That is not hypothetical —
    //    it is the defect this module was created for.
    std::string runtimeFallback;

    // 4. The loader tag (`--disable-new-dtags` / `--enable-new-dtags`).
    //    LITERALLY last, because ld honours the LAST one it sees and both gcc
    //    specs and clang config files supply the opposite of what mcpp wants.
    //    Nothing may be appended after this slot.
    std::string loaderTag;

    // Slots are concatenated in declaration order. A non-empty slot that does
    // not already begin with a separator gets one, so a producer cannot break
    // the line by forgetting the leading space (every producer today supplies
    // it, and the result is byte-identical to the hand-rolled concatenation
    // this replaced).
    std::string render() const {
        std::string out;
        for (const std::string* slot :
             {&dependencies, &cxxRuntime, &runtimeFallback, &loaderTag}) {
            if (slot->empty()) continue;
            if (!slot->starts_with(' ')) out += ' ';
            out += *slot;
        }
        return out;
    }

    bool empty() const {
        return dependencies.empty() && cxxRuntime.empty()
            && runtimeFallback.empty() && loaderTag.empty();
    }
};

}  // namespace mcpp::build::link_line
