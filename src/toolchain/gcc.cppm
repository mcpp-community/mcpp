// mcpp.toolchain.gcc - GCC-family compiler behavior.

export module mcpp.toolchain.gcc;

import std;
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

std::filesystem::path std_bmi_path(const std::filesystem::path& cacheDir);
std::filesystem::path staged_std_bmi_path(const std::filesystem::path& outputDir);

std::string std_module_build_command(const Toolchain& tc,
                                     const std::filesystem::path& cacheDir,
                                     std::string_view sysrootFlag);

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

std::filesystem::path std_bmi_path(const std::filesystem::path& cacheDir) {
    return cacheDir / "gcm.cache" / "std.gcm";
}

std::filesystem::path staged_std_bmi_path(const std::filesystem::path& outputDir) {
    return outputDir / "gcm.cache" / "std.gcm";
}

std::string std_module_build_command(const Toolchain& tc,
                                     const std::filesystem::path& cacheDir,
                                     std::string_view sysrootFlag) {
    std::string bFlag;
    if (auto binutilsBin = find_binutils_bin(tc.binaryPath)) {
        bFlag = std::format(" -B'{}'", binutilsBin->string());
    }

    return std::format(
        "cd {} && {}{} -std=c++23 -fmodules -O2{}{} -c {} -o std.o 2>&1",
        mcpp::xlings::shq(cacheDir.string()),
        mcpp::toolchain::compiler_env_prefix(tc),
        mcpp::xlings::shq(tc.binaryPath.string()),
        sysrootFlag,
        bFlag,
        mcpp::xlings::shq(tc.stdModuleSource.string()));
}

} // namespace mcpp::toolchain::gcc
