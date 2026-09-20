// THE MACROS mcpp ITSELF DEFINES, AND THE RULES FOR READING THEM.
//
// This module is the specification and the implementation of the same thing.
// The contract is `kContract` below --- data, enumerable, checked --- and the
// emission is `define_tokens`. `docs/21` renders the table for a reader;
// `tests/unit/test_predefines.cpp` asserts the two halves agree, so a macro
// added to one and not the other fails a build rather than drifting into a
// release.
//
// ── WHY AN ENGINE DEFINES ANY MACRO AT ALL ─────────────────────────────────
//
// Normally it should not. A package states what it needs in its manifest and
// the engine answers by RESOLUTION: `cfg(os = "windows")`, `cfg(c-abi =
// "musl")`, a capability, a feature. That path is testable, reportable, and
// visible to a reader of the manifest. A macro is none of those things, so
// every entry here has to justify itself against the manifest alternative.
//
// Exactly two justifications have survived:
//
//   1. THE SOURCE IS NOT OURS TO EDIT, and asks in the preprocessor. Upstream
//      C code selects platform behaviour with `#if`, and no manifest key can
//      reach inside a third-party `.c` file.
//
//   2. THE READER IS AN INSTALLED HEADER. A package's own build defines can
//      be spelled in its manifest, but a header it INSTALLS is read by an
//      application's own compile, which those defines never reach. Two such
//      headers exist in this ecosystem and both size records by the target's
//      ABI: `openkal-musl`'s `bits/setjmp.h` (`jmp_buf`) and
//      `openkal-llvm-runtime`'s `__libunwind_config.h` (`unw_context_t`).
//
// ── NAMING ─────────────────────────────────────────────────────────────────
//
// UPPER CASE, `__`-wrapped, words separated by `_`; the names mcpp owns carry
// `__MCPP_`.
//
// The convention in the wild splits by WHAT A NAME IS, not by who writes it.
// A vendor or product name is upper --- `__APPLE__`, `_WIN32`, `__MINGW32__`,
// `__GNUC__`. A kind-of-system name is lower --- `__linux__`, `__unix__`,
// `__gnu_linux__`. Every row this table OWNS is of the first kind: it names
// mcpp, or it names openkal. The kind-of-system question is answered by
// `__linux__` and its family, which mcpp SUPPLIES rather than owns and which
// therefore keep their lower-case spelling, for that exact reason.
//
// The lower-case spelling shipped first, and the reasoning that produced it
// is worth keeping because the mistake is easy to repeat: these names sit
// beside `__linux__` in real guards, so matching it looked like consistency.
// THAT CONFUSES ADJACENCY WITH KIND. `__APPLE__` sits in those same guards
// and is upper, because it belongs to somebody; so does every other name in
// them that belongs to somebody. Adjacency is not a naming rule.
//
// THE `__MCPP_` PREFIX IS LOAD-BEARING. A name mcpp owns means what mcpp says
// it means. The alternative was tried: `__CYGWIN__` was left defined so that
// code needing "PE object format, POSIX C environment" would have a name, and
// a 30-member measurement found four members reading it as "Win32 is
// available" and reaching `#include <windows.h>` --- which is what upstream
// means by it (mimalloc says so in the guard's own comment; sqlite3 lists it
// under `SQLITE_OS_WIN`). A BORROWED NAME MEANS WHAT THE LENDER'S HISTORY
// MADE IT MEAN, not what the borrower intended.
//
// ── STABILITY ──────────────────────────────────────────────────────────────
//
// An entry here is a published interface, and withdrawing one is SILENT: a
// `#if` selects the other branch and compiles, on a machine nobody is
// watching. No mechanism available to a build tool makes that loud --- a
// preprocessor cannot be told to complain about a name it does not find.
//
// So a withdrawal is never decided by reading. It is decided by A MEASUREMENT
// THAT ENUMERATES READERS, and what the measurement finds sets how many steps
// the withdrawal takes. Both withdrawals this project has performed are on
// the record, and they came out differently:
//
//   `__CYGWIN__` (2026.9.21.1). The enumeration found four third-party
//   members reading it, and --- the half a first, sloppier count missed ---
//   six sites in the two headers this ecosystem INSTALLS. Readers exist, so
//   the withdrawal was a sequence: publish the replacement, move the
//   consumers onto it while the old name is still defined, stop last.
//
//   `__mcpp_target_<os>__` and `__openkal__` (2026.9.21.2). The enumeration
//   found NONE. Across every repository of this ecosystem, not one source
//   file and not one manifest read either name; the only occurrences were
//   this engine's own emitter, its tests, and prose. Exposure was three days
//   for `__openkal__` and a single release for the target macro, and both are
//   names invented here, so no upstream code can be holding one. Zero readers
//   collapses the sequence to one step.
//
// The rule did not change between those two; the count did. A withdrawal
// argued from anything other than a count is an argument this project has
// already got wrong once.
export module mcpp.toolchain.predefines;

import std;

export namespace mcpp::toolchain::predefines {

// What decides whether an entry is defined for a given build.
enum class When {
    EveryTarget,   // always, for every target-side translation unit
    TargetOs,      // one per build, spelled from the triple's `os` field
    ResolvedLayer, // the resolved layer's interface name selects it
    Realisation,   // emitted by the `[c-abi]` realisation, not by this module
};

struct Entry {
    std::string_view spelling; // `<OS>` stands for the triple's `os` field
    When             when;
    bool             owned;    // false: a standard name mcpp SUPPLIES, not owns
    std::string_view allowed;
    std::string_view forbidden;
};

// THE COMPLETE SET. A macro mcpp defines and this table omits is a promise
// nobody can rely on; a row naming a macro nothing emits is one a reader will
// wait for forever. `test_predefines.cpp` compares the two directions.
inline constexpr std::array<Entry, 3> kContract {{
    {
        "__MCPP_TARGET_<OS>__", When::TargetOs, /*owned=*/true,
        "learning the target's operating system when the C environment "
        "presented above it has suppressed the platform's own macros, and "
        "sizing a record by the target's ABI",
        "selecting a header that the manifest could select, or standing in "
        "for `cfg(os = ...)` in a package this project controls",
    },
    {
        "__OPENKAL__", When::ResolvedLayer, /*owned=*/true,
        "gating whether a call site invokes `kal_*` at all; its meaning is "
        "identical on every target",
        "selecting a header, inferring whether `_WIN32` is real, working "
        "around a missing SDK, or telling linux/windows/macos apart",
    },
    {
        "__unix__", When::Realisation, /*owned=*/false,
        "reading it exactly as on any other POSIX system; mcpp SUPPLIES it "
        "where the target's own toolchain would not, because `[c-abi] "
        "presents = \"posix\"` says this environment is one",
        "treating its presence as an mcpp-specific signal --- it is the "
        "standard name, and mcpp neither owns it nor may change its meaning",
    },
}};

// ASCII upper case. The triple's `os` field is a closed vocabulary of
// lower-case identifier tokens --- `linux`, `windows`, `macos`, `ios`,
// `emscripten`, `none` --- every one of them assigned from a literal in the
// parser, so this always yields a valid identifier. That is a property of
// another module, so `test_predefines.cpp` asserts it over the TARGET
// REGISTRY rather than over this sentence.
inline std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

// THE TOKENS FOR ONE BUILD, for the entries this module emits.
//
// `When::Realisation` entries are absent by construction: they belong to
// `mcpp.toolchain.cenv`, which emits them as part of realising a declared
// `[c-abi]` block. They are listed in `kContract` because the contract is
// about what a READER may encounter, not about which module wrote it.
//
// NO OPERATING-SYSTEM NAME APPEARS IN THIS FUNCTION. The spelling comes from
// the triple's own `os` field, so a target added to the triple parser gets
// its macro with no change here --- the same discipline `[kernel-abi]`
// interface names follow.
inline std::vector<std::string> define_tokens(std::string_view targetOs,
                                              bool kernelAbiIsOpenkal) {
    std::vector<std::string> out;
    if (!targetOs.empty())
        out.push_back(std::format("-D__MCPP_TARGET_{}__=1", upper(targetOs)));
    if (kernelAbiIsOpenkal)
        out.push_back("-D__OPENKAL__");
    return out;
}

// The spelling `kContract` uses, for one concrete target. Exists so a test
// can compare the table against `define_tokens` without knowing either.
inline std::string spelling_for(const Entry& e, std::string_view targetOs) {
    std::string s(e.spelling);
    if (const auto at = s.find("<OS>"); at != std::string::npos)
        s.replace(at, 4, upper(targetOs));
    return s;
}

} // namespace mcpp::toolchain::predefines
