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
    std::string_view debugFlags;       // "-g"     | "/Z7"
    // `debug = "lines"`: the line tables alone (2026-11-12 §17, W5). `-g1` is
    // GCC's minimal level and clang's `-gline-tables-only`; cl.exe has no
    // level below /Z7.
    std::string_view lineDebugFlags;   // "-g1"    | "/Z7"
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
    // The MSVC CRT model for the OTHER linkage. Only meaningful on the MSVC
    // dialect; GNU has no counterpart (the empty string).
    std::string_view dynamicRuntime;   // ""       | "/MD"
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

    // How this archiver DELETES a member, as argv words before the member
    // names. `mcpp pack` needs it: a library package publishes its interface
    // units as source, so their objects have to come OUT of the archive, or the
    // consumer links two definitions of each published module's initialiser.
    //
    // It lives here rather than in the packer because "how do you speak to the
    // archiver" is exactly what this table is for. Hard-coding `ar`-style `d`
    // there would be right for GNU and llvm-ar and wrong for LIB.EXE, which
    // spells it `/REMOVE:<member>` and takes one flag per member — a difference
    // in ARITY as well as in spelling, which is why this is a template with a
    // placeholder rather than a prefix string.
    //
    // `{}` is substituted with the member name; the words are joined with
    // spaces after the archive path.
    std::string_view archiveRemoveArg; // "d" needs no {} | "/REMOVE:{}"
    bool             archiveRemoveTakesArchiveFirst = true;

    // THE IMPORT-LIBRARY AND `.def` SPELLINGS ARE NOT HERE, on purpose.
    //
    // They were, keyed on the dialect, and that is wrong in a way Windows CI
    // demonstrated: clang targeting the MSVC ABI speaks the GNU DIALECT while
    // driving LLD-LINK, so it was handed `-Wl,--out-implib,` and answered
    // `lld-link: warning: ignoring unknown argument '--out-implib'`. The
    // spelling follows the target ABI, which this table does not know — see
    // ninja_backend's pe_link_flag.
};

// Dialect lookup. GCC / Clang / MinGW → gnu; MSVC → msvc.
const CommandDialect& dialect_for(const Toolchain& tc);

// The MSVC CRT model for a given linkage, in ONE place.
//
// cl bakes `_MSVC_MT` / `_MSVC_MD` into every module it produces, so the std
// module and the TUs importing it must agree. They were derived in two places:
// the project's TUs from `flags.cppm` and the std module from cl's own default
// (`/MT`, because the std build passed no flag at all). A project on the default
// dynamic linkage therefore imported a `/MT` std, which cl accepts with a C5050
// warning and then fails on for real inside the ucrt headers — #422.
//
// Same shape as `macos_deployment_target`, which `stdmod::ensure_built` already
// takes for exactly this reason on the other platform.
constexpr std::string_view msvc_crt_flag(const CommandDialect& d, bool staticCrt) {
    return staticCrt ? d.staticRuntime : d.dynamicRuntime;
}

// Does this build want MSVC's STATIC CRT? TWO manifest keys answer it:
//
//   [build] linkage     = "static"           the libc axis
//   [build] cxx_runtime = "self-contained"   the C++ runtime axis
//
// That is not a redundancy to be picked between — on the MSVC ABI they are
// physically ONE switch. `/MT` links the C runtime and the C++ runtime out of
// the same library, and there is no way to have one static and the other
// dynamic. distribution.cppm's PE/MinGW branch already says exactly this
// about `-static`; MSVC is the same statement, and used to disagree with
// itself about it (`linkage` emitted /MT while `cxx_runtime` reported "not
// implemented" — for the same physical outcome).
//
// `cxxRuntime` is the RAW manifest scalar, and must stay that way: the
// resolved Contract defaults to SelfContained for most roles, so keying off
// it would flip every Windows build from /MD to /MT. Empty here means nobody
// wrote it down, which is the only reading under which "explicit" is honest.
//
// Asking the question once is also what keeps the project's TUs and the std
// module from disagreeing (#422): both callers ask THIS, rather than each
// spelling out `linkage == "static"` and drifting the next time a key is
// added — which is precisely how the second key came to be ignored.
constexpr bool msvc_wants_static_crt(std::string_view linkage,
                                     std::string_view cxxRuntime) {
    return linkage == "static" || cxxRuntime == "self-contained";
}

// THE CRT MODEL WORD, SPELLED FOR WHICHEVER DRIVER `tc` IS.
//
// Every MSVC-ABI row now receives the model — cl and clang++ targeting
// `*-windows-msvc` alike (#649 E10, #718) — because the CRT is a property of
// the target ABI, not of the compiler. `dialect_for` still gives clang the
// GNU dialect (its `-I`/`-D`/... spellings are unchanged), so this is NOT
// `msvc_crt_flag(dialect_for(tc), ...)`: the GNU dialect's own
// `staticRuntime` is `-static`, full static linking, a different axis this
// function must not be confused with.
//
//   cl.exe                    /MT            | /MD
//   clang++ *-windows-msvc    -fms-runtime-lib=static | -fms-runtime-lib=dll
//   every other row           "" (MinGW links the MSVC CRT to no row at all)
//
// ONE HELPER, so the translation units (flags.cppm), the std and std.compat
// BMIs (stdmod.cppm via clang.cppm/msvc.cppm) and the link command
// (flags.cppm's `LinkShape::PeLld` branch) cannot spell three different
// answers to the same question — which is exactly how E10 arose: a flag
// given only at compile time does not reach clang's link step, which chooses
// `-defaultlib:` on its own (measured, `2026-09-16-646-649-four-issues-by-
// home.md` §4.5).
std::string msvc_abi_crt_word(const Toolchain& tc, bool staticCrt);

// A FREE-FORM CRT WORD IS ALWAYS A SECOND STATEMENT (D3, #718).
//
// Every MSVC-ABI build now states its own CRT model, so a literal spelling of
// it in `[build] cxxflags` or `dialect_cxxflags` — cl's `/MT`/`/MD`(`d`) or
// clang's `-fms-runtime-lib=*` (either dash) — can never be the only voice:
// either it repeats what mcpp already resolved, or it contradicts it, and the
// engine must never let the last word on the command line decide silently.
//
// `word` is one token already read out of a flags list (SPEC-004 §8's
// per-word reading). `key` names where the caller found it, so the message
// can point back at it. Returns nullopt for a spelling this function does not
// recognise as a CRT word — it says nothing about flags outside this axis.
struct CrtWordVerdict {
    bool        contradicts;
    std::string message;
};
std::optional<CrtWordVerdict> check_crt_word(std::string_view word,
                                             bool staticCrt,
                                             std::string_view key);

// A SHARED-PDB WORD IS REFUSED ON cl.exe (2026.11.7.1). `/Zi` and `/ZI`
// write a compile's debug information into a PDB that every compile of a
// directory shares, through mspdbsrv.exe, a server the first cl.exe starts and
// that outlives it; `/FS` serialises those writes through it. Each compile
// runs in a process unit that retires with the compile, and the server is a
// member of the unit that started it: retiring that unit ends the server under
// every other compile writing through it. `[profile.<name>] debug = true`
// gives each object its own debug information (`/Z7`). Returns the refusal
// for such a word, either dash, and nullopt for any other word.
std::optional<std::string> check_shared_pdb_word(std::string_view word,
                                                 std::string_view key);

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
    .lineDebugFlags  = "-g1",
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
    // `ar d <archive> <member>...` — one verb, then every member.
    .archiveRemoveArg = "d",
    .archiveRemoveTakesArchiveFirst = true,
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
    // DEBUG INFORMATION IN EACH OBJECT (/Z7), not in a PDB that every compile
    // of a directory shares (/Zi /FS). The shared PDB is written through
    // mspdbsrv.exe, which the first cl.exe starts and which outlives it by
    // seconds; the compiler executor retires each compile's process unit, and
    // retiring the unit that holds the server ended the server under the
    // concurrent compiles writing through it (C1090, PDB API error 23). An
    // object that carries its own debug information is also complete on its
    // own, which the object cache and a taken-over build directory assume.
    .debugFlags      = "/Z7",
    .lineDebugFlags  = "/Z7",
    .alwaysFlags     = "/nologo /EHsc /utf-8",
    .alwaysFlagsArgv = kMsvcAlwaysArgv,
    .libFlag         = "{}.lib",
    .libSearchPrefix = "/LIBPATH:",
    .forceCxxLang    = "/TP",
    .forceCxxLangArgv = kMsvcForceCxxArgv,
    .perFileCxxPrefix = "/Tp",
    .staticRuntime   = "/MT",
    .dynamicRuntime  = "/MD",
    .outputExePrefix = "/Fe:",
    .objExt          = ".obj",
    .ninjaDepsMode   = "msvc",
    .rspfileLink     = true,
    .linkStyle       = CommandDialect::LinkStyle::SeparateLinker,
    .archiveCmd      = "$ar /nologo /OUT:$out $in",
    // `LIB /REMOVE:<member> … <archive>` — one flag PER member, and the
    // archive last. Untested against a real LIB.EXE (mcpp's Windows CI packs
    // with clang, whose llvm-ar takes the GNU form), so a failure here is
    // reported with the command that produced it rather than swallowed.
    .archiveRemoveArg = "/REMOVE:{}",
    .archiveRemoveTakesArchiveFirst = false,
};

} // namespace

const CommandDialect& dialect_for(const Toolchain& tc) {
    if (tc.compiler == CompilerId::MSVC) return kMsvcDialect;
    return kGnuDialect;
}

const CommandDialect& gnu_dialect()  { return kGnuDialect; }
const CommandDialect& msvc_dialect() { return kMsvcDialect; }

std::string msvc_abi_crt_word(const Toolchain& tc, bool staticCrt) {
    if (tc.compiler == CompilerId::MSVC)
        return std::string(msvc_crt_flag(msvc_dialect(), staticCrt));
    if (is_msvc_target(tc))
        return staticCrt ? "-fms-runtime-lib=static" : "-fms-runtime-lib=dll";
    return {};
}

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

std::optional<std::string> check_shared_pdb_word(std::string_view word,
                                                 std::string_view key) {
    if (word.size() != 3 || (word[0] != '/' && word[0] != '-'))
        return std::nullopt;
    const auto spelling = word.substr(1);
    if (spelling != "Zi" && spelling != "ZI" && spelling != "FS")
        return std::nullopt;
    return std::format(
        "`{}` in {} writes debug information through mspdbsrv.exe, a PDB server "
        "shared by every compile and ended with the compile that started it. "
        "Remove the flag: `[profile.<name>] debug = true` gives each object its "
        "own debug information (/Z7)",
        word, key);
}

std::optional<CrtWordVerdict> check_crt_word(std::string_view word,
                                             bool staticCrt,
                                             std::string_view key) {
    // Both dash conventions: cl.exe accepts `-MD` exactly as it does `/MD`,
    // and a manifest that targets more than one driver from one list (SPEC-
    // 004 §8) should not have to spell the word twice.
    std::optional<bool> wantsStatic;
    if (word == "/MT" || word == "-MT" || word == "/MTd" || word == "-MTd"
        || word == "-fms-runtime-lib=static"
        || word == "-fms-runtime-lib=static_dbg") {
        wantsStatic = true;
    } else if (word == "/MD" || word == "-MD" || word == "/MDd" || word == "-MDd"
              || word == "-fms-runtime-lib=dll"
              || word == "-fms-runtime-lib=dll_dbg") {
        wantsStatic = false;
    } else {
        return std::nullopt;
    }

    // A DEBUG CRT WORD IS NEVER REDUNDANT. The model states static or dynamic
    // and nothing about debug (that axis is deferred, docs/20), while the std
    // module and the link are built against the release CRT it resolved. A
    // debug word therefore compiles its units against a CRT nothing else uses.
    const bool debugWord = word.ends_with("d") || word.ends_with("_dbg");
    if (debugWord) {
        return CrtWordVerdict{true, std::format(
            "`{}` in {} asks for a debug CRT, which the CRT model does not "
            "express; the standard library module and the link use {}. Remove "
            "the flag",
            word, key, staticCrt ? "the static CRT, /MT" : "the dynamic CRT, /MD")};
    }
    std::string_view wordValue =
        *wantsStatic ? "the static CRT (/MT)" : "the dynamic CRT (/MD)";
    if (*wantsStatic == staticCrt) {
        return CrtWordVerdict{false, std::format(
            "`{}` in {} agrees with the CRT model this build already "
            "resolved and says nothing new. Write {} instead if {} should "
            "stay an explicit statement, and drop the flag",
            word, key,
            staticCrt ? "`cxx_runtime = \"self-contained\"` (or `linkage = \"static\"`)"
                      : "`cxx_runtime = \"toolchain-coupled\"` (or `\"host-coupled\"`)",
            wordValue)};
    }
    return CrtWordVerdict{true, std::format(
        "`{}` in {} asks for {}, which contradicts the CRT model this build "
        "already resolved ({}). Every MSVC-ABI row now states its own CRT, "
        "so the word can never be the only voice: remove it, or change "
        "`cxx_runtime`/`linkage` to match",
        word, key, wordValue,
        staticCrt ? "the static CRT, /MT" : "the dynamic CRT, /MD")};
}

} // namespace mcpp::toolchain
