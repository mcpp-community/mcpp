// mcpp.toolchain.model - stable toolchain data model.

export module mcpp.toolchain.model;

import std;
import mcpp.toolchain.triple;

export namespace mcpp::toolchain {

enum class CompilerId { Unknown, GCC, Clang, MSVC };

// Fine-grained sysroot paths derived from xpkgs payloads.
// When populated, flags are assembled from these paths instead of --sysroot.
// One environment variable a toolchain needs at tool-invocation time.
struct EnvVar {
    std::string key;
    std::string value;
};

// The runtime this toolchain builds AGAINST, in xlings's spelling
// ("glibc@2.39"). Resolved before payload probing from the root-selected
// RuntimeBinding snapshot. Empty means no libc payload applies, and payload
// resolution then declines rather than guessing.
struct PayloadPaths {
    std::filesystem::path glibcInclude;     // glibc headers (features.h, bits/)
    std::filesystem::path glibcLib;          // glibc runtime (libc.so, crt*.o, ld-linux)
    std::filesystem::path linuxInclude;      // linux kernel headers (linux/, asm/)
};

struct Toolchain {
    CompilerId                          compiler        = CompilerId::Unknown;
    std::string                         version;            // "15.1.0"
    std::filesystem::path               binaryPath;
    std::string                         driverIdent;        // normalized --version output
    std::string                         targetTriple;       // "x86_64-linux-gnu"
    // The runtime this toolchain builds AGAINST, in xlings's own spelling
    // ("glibc@2.39"). Resolved BEFORE payload probing from the root-selected
    // RuntimeBinding snapshot.
    //
    // Empty is a refusal, not a default: payload resolution declines rather
    // than picking a libc by directory order. That guess is what let the
    // compile side and the artifact's interpreter name different glibc
    // versions, with nothing in the resulting binary looking wrong until it
    // loaded a library built against the other one.
    std::string                         runtimeBinding;
    // Hash of the complete immutable RuntimeBinding snapshot (selection,
    // provider, runtime and environment), not merely its libc label. Two
    // named SubOS environments may both say glibc@2.39 and still require
    // different loader/driver contracts, so the build cache must separate
    // them. Empty keeps compatibility for low-level detector unit tests.
    std::string                         runtimeContractHash;
    std::string                         stdlibId;           // "libstdc++"
    std::string                         stdlibVersion;
    std::filesystem::path               stdModuleSource;    // bits/std.cc / std.cppm
    std::filesystem::path               stdCompatSource;    // bits/std_compat.cc / std.compat.cppm
    std::filesystem::path               sysroot;            // -print-sysroot output (or empty)
    std::optional<PayloadPaths>         payloadPaths;        // fine-grained sysroot from xpkgs
    std::vector<std::filesystem::path>   compilerRuntimeDirs; // LD_LIBRARY_PATH for private tools
    std::vector<std::filesystem::path>   linkRuntimeDirs;     // -L/-rpath dirs for produced binaries
    // Environment the toolchain's tools need when invoked (set on the ninja
    // process, inherited by compiler/linker children). Empty for GCC/Clang
    // (their LD_LIBRARY_PATH need goes through compilerRuntimeDirs); the
    // MSVC backend fills INCLUDE/LIB/PATH here (design §5.1).
    // (Own struct, not std::pair — GCC 16 modules choke on a std::pair
    // member added to this exported class: "failed to load pendings".)
    std::vector<EnvVar> envOverrides;
    bool                                hasImportStd = false;
    // Lowest -std= level this toolchain can build the std module at. 0 = the
    // provider did not say, callers fall back to the plain hasImportStd
    // question. Filled next to hasImportStd by each provider, because "which
    // std module does this compiler ship" is provider-local knowledge — a
    // central table would derive the same decision in a second place.
    // GCC/libc++ answer 20; MSVC answers 20 from cl 19.38 (VS 2022 17.8,
    // microsoft/STL#3977) and 23 below that.
    int                                 importStdMinLevel = 0;

    std::string label() const {
        return std::format("{} {} ({})", compiler_name(), version, targetTriple);
    }

    std::string_view compiler_name() const {
        switch (compiler) {
            case CompilerId::GCC:   return "gcc";
            case CompilerId::Clang: return "clang";
            case CompilerId::MSVC:  return "msvc";
            default:                return "unknown";
        }
    }
};

struct DetectError { std::string message; };

bool is_gcc(const Toolchain& tc);
bool is_clang(const Toolchain& tc);
bool is_musl_target(const Toolchain& tc);
bool is_msvc_target(const Toolchain& tc);
bool is_mingw_target(const Toolchain& tc);

// Can the artifact we are building be fully statically linked (`-static`)?
//
// This is a property of the TARGET, not of the machine doing the build —
// a Windows host cross-compiling to x86_64-linux-musl still produces a fully
// static ELF. `mcpp::platform::supports_full_static` answers a DIFFERENT
// question ("can THIS machine's own binaries be static"), and using it here
// silently dropped `-static` from every Windows→Linux cross build.
//
// `hostCapability` is threaded in explicitly rather than read from
// mcpp::platform so the decision is testable on any host: passing false
// models a Windows/macOS host. It is only consulted for the host target
// (empty triple), where target *is* host. Callers pass
// mcpp::platform::supports_full_static — keeping that dependency at the call
// site leaves this module free of any platform import.
bool target_supports_full_static(std::string_view targetTriple, bool hostCapability);

struct BmiTraits {
    std::string_view bmiDir;     // "gcm.cache" | "pcm.cache" | "ifc.cache"
    std::string_view bmiExt;     // ".gcm"      | ".pcm"      | ".ifc"
    std::string_view manifestPrefix; // "gcm"   | "pcm"       | "ifc"
    bool needsExplicitModuleOutput = false;
    bool needsPrebuiltModulePath = false;
    bool scanNeedsFModules = true;
    // Module-flag spellings (leading space included; empty = not emitted).
    std::string_view compileModulesFlag;    // " -fmodules" (GCC) | ""
    std::string_view stdBmiUsePrefix;       // "" | " -fmodule-file=std=" | " /reference std="
    std::string_view stdCompatBmiUsePrefix; // "" | " -fmodule-file=std.compat=" | " /reference std.compat="
    std::string_view moduleOutputPrefix;    // "" | " -fmodule-output=" | " /ifcOutput "
    std::string_view bmiSearchPrefix;       // "" | " -fprebuilt-module-path=" | " /ifcSearchDir "
    // How this compiler is TOLD that a translation unit is a module interface.
    // Emitted UNCONDITIONALLY on every module compile — mcpp never asks
    // "does this driver recognize this extension?".
    //
    // WHY UNCONDITIONALLY. Every driver has a private, version-dependent
    // suffix->language table, and the three disagree in both directions:
    // measured 2026-08-11, Clang 22.1.8 does not recognize `.ixx` at all
    // (it hands the file to the LINKER, warns, and exits 0 with no BMI —
    // a silent no-op), while cl.exe does not recognize `.cppm`. Maintaining
    // "who knows which suffix" would be a table that expires with every
    // compiler release AND whose errors are silent.
    //
    // Saying it every time costs nothing: the flag is IDEMPOTENT on a suffix
    // the driver already knows. Measured on the same day — Clang's `.cppm`
    // BMI is byte-identical with and without `-x c++-module` (18896 bytes
    // both); GCC's `.gcm` is unchanged too (its output is not byte-
    // reproducible run to run, so the comparison is same-size plus a diff
    // offset indistinguishable from the run-to-run noise).
    //
    // NOT interchangeable between families: `-x c++-module` makes GCC exit
    // with "language c++-module not recognized", and `-x c++` makes Clang
    // emit a 174-byte stub instead of a module BMI.
    //
    // Positional on GNU, so the emitter must place it before `-c $in`.
    std::string_view moduleInterfaceLangFlag; // " -x c++" | " -x c++-module" | " /interface /TP"
};

BmiTraits bmi_traits(const Toolchain& tc);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

bool is_gcc(const Toolchain& tc) {
    return tc.compiler == CompilerId::GCC;
}

bool is_clang(const Toolchain& tc) {
    return tc.compiler == CompilerId::Clang;
}

// Target-shape predicates read the parsed canonical Triple (triple.cppm is
// the single triple parser), with the old substring heuristics kept only as
// a fallback for triples outside the language. This makes them spelling-
// independent: "x86_64-w64-mingw32" and canonical "x86_64-windows-gnu" give
// the same answer.
bool is_musl_target(const Toolchain& tc) {
    if (auto t = triple::parse(tc.targetTriple)) return t->is_musl();
    return tc.targetTriple.find("-musl") != std::string::npos;
}

bool is_msvc_target(const Toolchain& tc) {
    if (auto t = triple::parse(tc.targetTriple)) return t->is_msvc_env();
    return tc.targetTriple.find("msvc") != std::string::npos;
}

bool is_mingw_target(const Toolchain& tc) {
    if (auto t = triple::parse(tc.targetTriple)) return t->is_windows_gnu();
    // "x86_64-w64-mingw32" (mingw-w64) / legacy "*-pc-mingw32".
    return tc.targetTriple.find("mingw32") != std::string::npos;
}

bool target_supports_full_static(std::string_view targetTriple, bool hostCapability) {
    // Empty triple means "build for this machine" — target IS host, so the
    // host answer is the correct one. This is the only case where the host
    // capability legitimately decides.
    if (targetTriple.empty()) return hostCapability;

    auto t = triple::parse(targetTriple);
    // Outside the triple language we have nothing to reason from. Fall back
    // to the host answer rather than guessing from a substring — a wrong
    // `true` here would emit `-static` at a target that cannot honour it.
    if (!t) return hostCapability;

    // PE targets get their `-static` from the C++ runtime distribution
    // contract (dist::Format::Pe in flags.cppm), never from here. Returning
    // false is what keeps the two mechanisms from both emitting the flag.
    if (t->is_pe()) return false;

    // macOS cannot fully static-link: libSystem must stay dynamic.
    if (t->os == "macos") return false;

    // Linux ELF — glibc or musl, native or cross. This is the line that was
    // previously gated on the HOST being Linux.
    return t->os == "linux";
}

BmiTraits bmi_traits(const Toolchain& tc) {
    if (tc.compiler == CompilerId::MSVC) {
        // Native cl.exe builds are gated off until the .ifc pipeline lands;
        // these traits exist so nothing silently reuses the GCC defaults.
        return {
            .bmiDir = "ifc.cache",
            .bmiExt = ".ifc",
            .manifestPrefix = "ifc",
            .needsExplicitModuleOutput = true,
            .needsPrebuiltModulePath = true,
            .scanNeedsFModules = false,
            .compileModulesFlag = "",
            .stdBmiUsePrefix = " /reference std=",
            .stdCompatBmiUsePrefix = " /reference std.compat=",
            .moduleOutputPrefix = " /ifcOutput ",
            .bmiSearchPrefix = " /ifcSearchDir ",
            // Pre-existing behaviour, unchanged: cl has always been told
            // explicitly, because mcpp's interfaces are `.cppm` and cl does
            // not know that suffix. The other two families now match it.
            .moduleInterfaceLangFlag = " /interface /TP",
        };
    }
    if (is_clang(tc)) {
        return {
            .bmiDir = "pcm.cache",
            .bmiExt = ".pcm",
            .manifestPrefix = "pcm",
            .needsExplicitModuleOutput = true,
            .needsPrebuiltModulePath = true,
            .scanNeedsFModules = false,
            .compileModulesFlag = "",
            .stdBmiUsePrefix = " -fmodule-file=std=",
            .stdCompatBmiUsePrefix = " -fmodule-file=std.compat=",
            .moduleOutputPrefix = " -fmodule-output=",
            .bmiSearchPrefix = " -fprebuilt-module-path=",
            .moduleInterfaceLangFlag = " -x c++-module",
        };
    }
    return {
        .bmiDir = "gcm.cache",
        .bmiExt = ".gcm",
        .manifestPrefix = "gcm",
        .needsExplicitModuleOutput = false,
        .needsPrebuiltModulePath = false,
        .scanNeedsFModules = true,
        // GCC: -fmodules on every TU; BMIs implicit in cwd/gcm.cache, no
        // std=/search flags needed.
        .compileModulesFlag = " -fmodules",
        .stdBmiUsePrefix = "",
        .stdCompatBmiUsePrefix = "",
        .moduleOutputPrefix = "",
        .bmiSearchPrefix = "",
        // GCC decides interface-ness from the content (`export module`), so
        // it only needs to be told the LANGUAGE. `-x c++-module` is not a
        // value GCC accepts.
        .moduleInterfaceLangFlag = " -x c++",
    };
}

} // namespace mcpp::toolchain
