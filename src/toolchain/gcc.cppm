// mcpp.toolchain.gcc - GCC-family compiler behavior.

export module mcpp.toolchain.gcc;

import std;
import mcpp.platform;
import mcpp.toolchain.model;
import mcpp.toolchain.probe;
import mcpp.xlings;

export namespace mcpp::toolchain::gcc {

bool matches_version_output(std::string_view firstLineLower);
std::string parse_version(std::string_view firstLine);

std::optional<std::filesystem::path> find_std_module_source(
    const std::filesystem::path& cxx_binary,
    std::string_view version);

void enrich_toolchain(Toolchain& tc);

std::optional<std::filesystem::path>
find_binutils_bin(const std::filesystem::path& compilerBin);

// The external GNU binutils directory this toolchain must be pointed at with
// `-B`, or empty when it must not be pointed at one — which is every toolchain
// that is not a glibc GCC.
//
// ONE STATEMENT OF A GUARD THAT HAD THREE COPIES. musl-cross-make and
// MinGW-w64 payloads bundle their own as/ld, and for a cross target the host's
// binutils would mis-assemble — the Linux `as` rejects MinGW's PE/SEH
// directives `.def` / `.seh_proc`. Only the glibc GCC needs the external
// package. Each copy of that sentence was a place the next reader had to
// re-derive it, and one of them said so in a comment.
std::filesystem::path binutils_prefix_dir(const Toolchain& tc);

std::filesystem::path std_bmi_path(const std::filesystem::path& cacheDir);
std::filesystem::path staged_std_bmi_path(const std::filesystem::path& outputDir);

std::string std_module_build_command(const Toolchain& tc,
                                     const std::filesystem::path& cacheDir,
                                     std::string_view sysrootFlag,
                                     std::string_view cppStandardFlag);

// Normalized backend surface: same shape as clang::std_module_build_commands
// (a command sequence), so consumers don't branch on arity per compiler.
std::vector<std::string> std_module_build_commands(
    const Toolchain& tc,
    const std::filesystem::path& cacheDir,
    std::string_view sysrootFlag,
    std::string_view cppStandardFlag);

} // namespace mcpp::toolchain::gcc

namespace mcpp::toolchain::gcc {

bool matches_version_output(std::string_view firstLineLower) {
    return firstLineLower.find("g++") != std::string::npos
        || firstLineLower.find("gcc") != std::string::npos;
}

std::string parse_version(std::string_view firstLine) {
    std::string head(firstLine);
    auto rpos = head.find_last_of("0123456789");
    if (rpos != std::string::npos) {
        std::size_t lpos = rpos;
        while (lpos > 0
            && (std::isdigit(static_cast<unsigned char>(head[lpos - 1]))
                || head[lpos - 1] == '.')) {
            --lpos;
        }
        auto version = head.substr(lpos, rpos - lpos + 1);
        if (!version.empty()) return version;
    }
    return mcpp::toolchain::extract_version(firstLine);
}

std::optional<std::filesystem::path> find_std_module_source(
    const std::filesystem::path& cxx_binary,
    std::string_view version)
{
    auto root = cxx_binary.parent_path().parent_path();
    auto p = root / "include" / "c++" / std::string(version) / "bits" / "std.cc";
    if (std::filesystem::exists(p)) return p;

    // Cross toolchains (e.g. MinGW-w64 `x86_64-w64-mingw32-g++`) install the
    // TARGET libstdc++ — and its bits/std.cc — under a target-triple subdir:
    //   <prefix>/<triple>/include/c++/<version>/bits/std.cc
    // rather than the host-native <prefix>/include/c++/. Derive the triple from
    // the frontend filename (strip the -g++/-gcc/-c++ suffix) and look there.
    {
        std::error_code ec;
        auto stem = cxx_binary.stem().string();
        for (std::string_view suf : {"-g++", "-gcc", "-c++"}) {
            if (stem.size() > suf.size() && stem.ends_with(suf)) {
                std::string triple = stem.substr(0, stem.size() - suf.size());
                auto base = root / triple / "include" / "c++";
                auto pv = base / std::string(version) / "bits" / "std.cc";
                if (std::filesystem::exists(pv, ec)) return pv;
                if (std::filesystem::exists(base, ec)) {
                    for (auto& entry : std::filesystem::directory_iterator(base, ec)) {
                        auto cand = entry.path() / "bits" / "std.cc";
                        if (std::filesystem::exists(cand, ec)) return cand;
                    }
                }
                break;
            }
        }
    }

    // Version-dir scan fallback: the header dir doesn't always equal the
    // full driver version (e.g. distro / MinGW-w64 builds using the major
    // version, or a patched banner). Any bits/std.cc under include/c++ is
    // this installation's — take the first.
    {
        std::error_code ec;
        auto cxxInclude = root / "include" / "c++";
        if (std::filesystem::exists(cxxInclude, ec)) {
            for (auto& entry : std::filesystem::directory_iterator(cxxInclude, ec)) {
                auto cand = entry.path() / "bits" / "std.cc";
                if (std::filesystem::exists(cand, ec)) return cand;
            }
        }
    }

    auto cmd = std::format("'{}' -print-file-name=libstdc++.so 2>/dev/null",
                           cxx_binary.string());
    auto r = mcpp::toolchain::run_capture(cmd);
    if (r) {
        auto trimmed = mcpp::toolchain::trim_line(*r);
        if (!trimmed.empty()) {
            std::filesystem::path libpath = trimmed;
            auto root2 = libpath.parent_path().parent_path();
            auto p2 = root2 / "include" / "c++" / std::string(version) / "bits" / "std.cc";
            if (std::filesystem::exists(p2)) return p2;
        }
    }
    return std::nullopt;
}

void enrich_toolchain(Toolchain& tc) {
    tc.stdlibId      = "libstdc++";
    tc.stdlibVersion = tc.version;
    if (auto p = find_std_module_source(tc.binaryPath, tc.version)) {
        tc.stdModuleSource = *p;
        tc.hasImportStd    = true;
        // libstdc++'s bits/std.cc carries no __cplusplus guard: any GCC that
        // ships it builds the std module at C++20 too. Verified on gcc 15.1.0
        // and 16.1.0 across glibc / musl / mingw-cross targets (design §2.2).
        tc.importStdMinLevel = 20;
    }
}

std::optional<std::filesystem::path>
find_binutils_bin(const std::filesystem::path& compilerBin) {
    if (auto as = mcpp::xlings::paths::find_sibling_binary(
            compilerBin, "binutils", "bin/as")) {
        return as->parent_path();
    }
    return std::nullopt;
}

std::filesystem::path binutils_prefix_dir(const Toolchain& tc) {
    // GCC only, and the restriction is what makes the name true. Clang resolves
    // its assembler and linker through its own payload and must never be handed
    // a GNU binutils directory; the engine's clang command lines carry no `-B`
    // at all, so answering with one would describe a flag nobody passes.
    if (tc.compiler != CompilerId::GCC) return {};
    if (is_musl_target(tc) || is_mingw_target(tc)) return {};
    if (auto bin = find_binutils_bin(tc.binaryPath)) return *bin;
    return {};
}

std::filesystem::path std_bmi_path(const std::filesystem::path& cacheDir) {
    return cacheDir / "gcm.cache" / "std.gcm";
}

std::filesystem::path staged_std_bmi_path(const std::filesystem::path& outputDir) {
    return outputDir / "gcm.cache" / "std.gcm";
}

std::string std_module_build_command(const Toolchain& tc,
                                     const std::filesystem::path& cacheDir,
                                     std::string_view sysrootFlag,
                                     std::string_view cppStandardFlag) {
    std::string bFlag;
    if (auto binutilsBin = binutils_prefix_dir(tc); !binutilsBin.empty())
        bFlag = std::format(" -B{}", mcpp::xlings::shq(binutilsBin.string()));

    // Windows (MinGW): cmd.exe needs `/d` to change DRIVE (project on D:,
    // BMI cache on C: is the real CI layout — same-drive runs masked this).
    const char* cd = mcpp::platform::is_windows ? "cd /d" : "cd";
    return std::format(
        "{} {} && {}{} {} -fmodules -O2{}{} -c {} -o std.o 2>&1",
        cd,
        mcpp::xlings::shq(cacheDir.string()),
        mcpp::toolchain::compiler_env_prefix(tc),
        mcpp::xlings::shq(tc.binaryPath.string()),
        cppStandardFlag,
        sysrootFlag,
        bFlag,
        mcpp::xlings::shq(tc.stdModuleSource.string()));
}

std::vector<std::string> std_module_build_commands(
    const Toolchain& tc,
    const std::filesystem::path& cacheDir,
    std::string_view sysrootFlag,
    std::string_view cppStandardFlag) {
    return { std_module_build_command(tc, cacheDir, sysrootFlag, cppStandardFlag) };
}

} // namespace mcpp::toolchain::gcc
