// mcpp.toolchain.model - stable toolchain data model.

export module mcpp.toolchain.model;

import std;

export namespace mcpp::toolchain {

enum class CompilerId { Unknown, GCC, Clang, MSVC };

// Fine-grained sysroot paths derived from xpkgs payloads.
// When populated, flags are assembled from these paths instead of --sysroot.
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

struct BmiTraits {
    std::string_view bmiDir;     // "gcm.cache" | "pcm.cache"
    std::string_view bmiExt;     // ".gcm"      | ".pcm"
    std::string_view manifestPrefix; // "gcm"   | "pcm"
    bool needsExplicitModuleOutput = false;
    bool needsPrebuiltModulePath = false;
    bool scanNeedsFModules = true;
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

bool is_musl_target(const Toolchain& tc) {
    return tc.targetTriple.find("-musl") != std::string::npos;
}

bool is_msvc_target(const Toolchain& tc) {
    return tc.targetTriple.find("msvc") != std::string::npos;
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
            .needsPrebuiltModulePath = false,
            .scanNeedsFModules = false,
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
        };
    }
    return {
        .bmiDir = "gcm.cache",
        .bmiExt = ".gcm",
        .manifestPrefix = "gcm",
        .needsExplicitModuleOutput = false,
        .needsPrebuiltModulePath = false,
        .scanNeedsFModules = true,
    };
}

} // namespace mcpp::toolchain
