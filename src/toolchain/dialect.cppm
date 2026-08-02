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
    // Same rationale as forceCxxLangArgv: the argv consumer gets tokens, not
    // a string it has to split. Both spans point at static arrays below.
    std::span<const std::string_view> alwaysFlagsArgv;

    // Link and language-selection spellings.
    //
    // `libFlag` is a FORMAT, not a prefix: GNU names a library by prefixing
    // (`-lz`) while MSVC names it by suffixing (`z.lib`), and no single
    // prefix string can express both. Use lib_flag_for().
    std::string_view libFlag;          // "-l{}"   | "{}.lib"
    std::string_view libSearchPrefix;  // "-L"     | "/LIBPATH:"
    // The `.mcpp` extension is unknown to every compiler driver, so the
    // language has to be forced or the driver hands the file to the linker.
    //
    // Two forms of the same thing, because the two consumers need different
    // shapes and neither should re-derive the other's: a ninja command line
    // wants one string, an argv vector wants tokens. Storing both keeps the
    // spelling in one row — splitting the string at the call site would put
    // the token boundary in a second place, and cost a helper this file is
    // better off without (see the note on `alwaysFlagsArgv`).
    std::string_view forceCxxLang;     // "-x c++" | "/TP"
    std::span<const std::string_view> forceCxxLangArgv;
    // Per-FILE language force, for a command line that also carries object
    // files. The two drivers differ structurally, not just in spelling: GNU's
    // `-x c++` is positional and stays in effect until `-x none`, while
    // cl.exe's `/TP` applies to EVERY input — so an object listed after it is
    // fed to the C++ frontend and dies with C2018. cl's per-file form is
    // `/Tp<file>`; GNU has none, and uses the positional pair plus a reset.
    // Empty means "no per-file form — use forceCxxLangArgv and reset after".
    std::string_view perFileCxxPrefix; // ""       | "/Tp"
    // Static CRT / runtime. On MSVC this is a compile-time CRT model, not a
    // link mode — there is no /MT equivalent of `-static` for the whole image.
    std::string_view staticRuntime;    // "-static"| "/MT"
    // Output an executable (linking driver step).
    std::string_view outputExePrefix;  // "-o "    | "/Fe:"

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

// The two dialect rows, reachable without a Toolchain. Exposed so the MSVC
// row — which no build reaches until the cl.exe backend lands — can still be
// unit-tested, and so callers that already know the shape they want (the
// build.mcpp host compile) need not synthesize a Toolchain to ask.
const CommandDialect& gnu_dialect();
const CommandDialect& msvc_dialect();

// Name a library the way this dialect does: `-lz` vs `z.lib`.
std::string lib_flag_for(const CommandDialect& d, std::string_view name);

// The full -std=/-/std: flag for a normalized standard (canonical like
// "c++26"/"gnu++23", numeric level). MSVC: /std:c++20 exists; everything
// newer maps to /std:c++latest (required for import std); gnu dialects have
// no MSVC equivalent and take the same mapping.
std::string std_flag_for(const CommandDialect& d,
                         std::string_view canonical, int level);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

namespace {

// Token forms of the multi-token rows. Static arrays so the spans above are
// constexpr-initializable and no consumer has to split a string at runtime.
constexpr std::string_view kGnuForceCxxArgv[]  = {"-x", "c++"};
constexpr std::string_view kMsvcForceCxxArgv[] = {"/TP"};
constexpr std::string_view kMsvcAlwaysArgv[]   = {"/nologo", "/EHsc", "/utf-8"};

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
    .alwaysFlagsArgv = {},
    .libFlag         = "-l{}",
    .libSearchPrefix = "-L",
    .forceCxxLang    = "-x c++",
    .forceCxxLangArgv = kGnuForceCxxArgv,
    .perFileCxxPrefix = "",
    .staticRuntime   = "-static",
    .outputExePrefix = "-o ",
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
    .alwaysFlagsArgv = kMsvcAlwaysArgv,
    .libFlag         = "{}.lib",
    .libSearchPrefix = "/LIBPATH:",
    .forceCxxLang    = "/TP",
    .forceCxxLangArgv = kMsvcForceCxxArgv,
    .perFileCxxPrefix = "/Tp",
    .staticRuntime   = "/MT",
    .outputExePrefix = "/Fe:",
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

const CommandDialect& gnu_dialect()  { return kGnuDialect; }
const CommandDialect& msvc_dialect() { return kMsvcDialect; }

std::string lib_flag_for(const CommandDialect& d, std::string_view name) {
    // Two shapes, one table entry: `{}` marks where the name goes, which is
    // a prefix position for GNU and a suffix position for MSVC.
    std::string out(d.libFlag);
    if (auto p = out.find("{}"); p != std::string::npos)
        out.replace(p, 2, name);
    return out;
}

std::string std_flag_for(const CommandDialect& d,
                         std::string_view canonical, int level) {
    if (d.id == "msvc") {
        if (level <= 20) return "/std:c++20";
        return "/std:c++latest";
    }
    return std::format("{}{}", d.stdPrefix, canonical);
}

} // namespace mcpp::toolchain
