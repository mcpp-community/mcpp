// mcpp.toolchain.dialect — command-line *spelling* per compiler family.
//
// The single data point for how a flag is spelt: GNU driver style
// (GCC / Clang / MinGW share one instance) vs MSVC cl.exe style. Consumers
// (flags.cppm, ninja_backend.cppm, plan.cppm) concatenate these fragments
// instead of hardcoding "-I"/"-o"/"ar rcs" — adding a compiler family is a
// second row of data, not another if/else at every call site.
//
// Deliberately a value-type aggregate, not a class hierarchy — same
// rationale as BmiTraits (2026-05-15-clang-parity-and-toolchain-abstraction
// §2.3): the differences are strings and booleans, not behavior.
//
// See .agents/docs/2026-07-13-toolchain-backend-abstraction-msvc-mingw-design.md §2.1.

export module mcpp.toolchain.dialect;

import std;
import mcpp.toolchain.model;

export namespace mcpp::toolchain {

struct CommandDialect {
    std::string_view id;               // "gnu" | "msvc"

    // Flag spellings. Prefixes are concatenated with their (already
    // ninja-escaped) argument by the caller.
    std::string_view includePrefix;    // "-I"     | "/I"
    std::string_view definePrefix;     // "-D"     | "/D"
    std::string_view stdPrefix;        // "-std="  | "/std:"
    std::string_view compileOnly;      // "-c"     | "/c"
    std::string_view outputObjPrefix;  // "-o "    | "/Fo:"
    std::string_view optPrefix;        // "-O"     | "/O"
    std::string_view debugFlags;       // "-g"     | "/Zi /FS"
    std::string_view alwaysFlags;      // ""       | "/nologo /EHsc /utf-8"

    // Artifact naming.
    std::string_view objExt;           // ".o"     | ".obj"

    // Ninja integration.
    // deps mode for header dependencies ("" = none today for gnu — module
    // deps go through the P1689/dyndep pipeline instead).
    std::string_view ninjaDepsMode;    // ""       | "msvc"
    // Whether link/archive command lines must go through a response file
    // (Windows 8191-char command-line limit; cl/link/lib accept @file).
    bool rspfileLink = false;

    // Link step shape: the compiler driver links (g++/clang++ $in -o $out)
    // or a separate linker is invoked (link.exe /OUT:).
    enum class LinkStyle { Driver, SeparateLinker };
    LinkStyle linkStyle = LinkStyle::Driver;

    // Full ninja command template for static archives.
    std::string_view archiveCmd;       // "$ar rcs $out $in" | "$ar /nologo /OUT:$out $in"
};

// Dialect lookup. GCC / Clang / MinGW → gnu; MSVC → msvc.
const CommandDialect& dialect_for(const Toolchain& tc);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

namespace {

constexpr CommandDialect kGnuDialect{
    .id              = "gnu",
    .includePrefix   = "-I",
    .definePrefix    = "-D",
    .stdPrefix       = "-std=",
    .compileOnly     = "-c",
    .outputObjPrefix = "-o ",
    .optPrefix       = "-O",
    .debugFlags      = "-g",
    .alwaysFlags     = "",
    .objExt          = ".o",
    .ninjaDepsMode   = "",
    .rspfileLink     = false,
    .linkStyle       = CommandDialect::LinkStyle::Driver,
    .archiveCmd      = "$ar rcs $out $in",
};

// Native cl.exe. Unreachable in builds until the MSVC backend lands
// (prepare.cppm gates CompilerId::MSVC) — the row exists so the data model
// is complete and unit-tested ahead of that work.
constexpr CommandDialect kMsvcDialect{
    .id              = "msvc",
    .includePrefix   = "/I",
    .definePrefix    = "/D",
    .stdPrefix       = "/std:",
    .compileOnly     = "/c",
    .outputObjPrefix = "/Fo:",
    .optPrefix       = "/O",
    .debugFlags      = "/Zi /FS",
    .alwaysFlags     = "/nologo /EHsc /utf-8",
    .objExt          = ".obj",
    .ninjaDepsMode   = "msvc",
    .rspfileLink     = true,
    .linkStyle       = CommandDialect::LinkStyle::SeparateLinker,
    .archiveCmd      = "$ar /nologo /OUT:$out $in",
};

} // namespace

const CommandDialect& dialect_for(const Toolchain& tc) {
    if (tc.compiler == CompilerId::MSVC) return kMsvcDialect;
    return kGnuDialect;
}

} // namespace mcpp::toolchain
