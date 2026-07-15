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
    };
}

} // namespace mcpp::toolchain
