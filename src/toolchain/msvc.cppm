// mcpp.toolchain.msvc — MSVC / Visual Studio discovery on Windows.
//
// Provides reliable discovery of Visual Studio installations and MSVC
// toolchain components (std.ixx, cl.exe, lib.exe, etc.) using multiple
// strategies:
//   1. vswhere.exe (Microsoft's official VS locator)
//   2. Environment variables (VSINSTALLDIR, VS*COMNTOOLS)
//   3. Well-known installation paths (fallback)
//
// This module is used by clang.cppm to find MSVC STL's std.ixx when
// Clang targets x86_64-pc-windows-msvc. It will also serve as the
// foundation for future native MSVC (cl.exe) toolchain support.

module;
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#define popen  _popen
#define pclose _pclose
#endif

export module mcpp.toolchain.msvc;

import std;

export namespace mcpp::toolchain::msvc {

// Find a Visual Studio installation path (returns the newest found).
std::optional<std::filesystem::path> find_vs_install_path();

// Find the MSVC tools directory: <VS>/VC/Tools/MSVC/<latest_version>/
std::optional<std::filesystem::path> find_msvc_tools_dir();

// Find MSVC STL's std.ixx module source file.
std::optional<std::filesystem::path> find_std_module_source();

// Find cl.exe (for future MSVC toolchain support).
std::optional<std::filesystem::path> find_cl();

} // namespace mcpp::toolchain::msvc

namespace mcpp::toolchain::msvc {

namespace {

#if defined(_WIN32)

// Run a command and capture stdout (first line, trimmed).
std::string run_capture_line(const std::string& cmd) {
    std::array<char, 4096> buf{};
    std::string out;
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return {};
    while (std::fgets(buf.data(), buf.size(), fp) != nullptr)
        out += buf.data();
    ::pclose(fp);
    // Trim trailing whitespace/newlines
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    // Take first line only
    auto nl = out.find('\n');
    if (nl != std::string::npos) out.resize(nl);
    return out;
}

// Strategy 1: Use vswhere.exe to find VS installation.
std::optional<std::filesystem::path> find_vs_via_vswhere() {
    // vswhere.exe ships with the VS Installer at a well-known path
    std::filesystem::path vswhere =
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (!std::filesystem::exists(vswhere)) return std::nullopt;

    auto result = run_capture_line(
        "\"" + vswhere.string() + "\" -latest -products * "
        "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
        "-property installationPath 2>nul");

    if (!result.empty() && std::filesystem::exists(result))
        return std::filesystem::path(result);
    return std::nullopt;
}

// Strategy 2: Use environment variables.
std::optional<std::filesystem::path> find_vs_via_env() {
    // VSINSTALLDIR is set inside VS Developer Command Prompt
    if (auto* dir = std::getenv("VSINSTALLDIR"); dir && *dir) {
        std::filesystem::path p{dir};
        if (std::filesystem::exists(p / "VC" / "Tools" / "MSVC"))
            return p;
    }

    // VS*COMNTOOLS: VS170COMNTOOLS (2022), VS160COMNTOOLS (2019), VS150COMNTOOLS (2017)
    for (auto* var : {"VS170COMNTOOLS", "VS160COMNTOOLS", "VS150COMNTOOLS"}) {
        if (auto* val = std::getenv(var); val && *val) {
            // Common7/Tools/ → go up two levels to VS root
            std::filesystem::path p{val};
            auto root = p.parent_path().parent_path();
            if (std::filesystem::exists(root / "VC" / "Tools" / "MSVC"))
                return root;
        }
    }
    return std::nullopt;
}

// Strategy 3: Scan well-known paths.
std::optional<std::filesystem::path> find_vs_via_paths() {
    static constexpr std::string_view bases[] = {
        "C:\\Program Files\\Microsoft Visual Studio",
        "C:\\Program Files (x86)\\Microsoft Visual Studio",
    };
    static constexpr std::string_view years[] = {"2025", "2022", "2019", "2017"};
    static constexpr std::string_view editions[] = {
        "Enterprise", "Professional", "Community", "BuildTools", "Preview"
    };

    std::error_code ec;
    for (auto base : bases) {
        for (auto year : years) {
            for (auto edition : editions) {
                auto p = std::filesystem::path(base) / std::string(year) / std::string(edition);
                if (std::filesystem::exists(p / "VC" / "Tools" / "MSVC", ec))
                    return p;
            }
        }
    }
    return std::nullopt;
}

// From a VS install path, find the latest MSVC tools version directory.
std::optional<std::filesystem::path> find_latest_msvc_tools(const std::filesystem::path& vsRoot) {
    auto vcTools = vsRoot / "VC" / "Tools" / "MSVC";
    std::error_code ec;
    if (!std::filesystem::exists(vcTools, ec)) return std::nullopt;

    std::filesystem::path latest;
    std::string latestVer;
    for (auto& entry : std::filesystem::directory_iterator(vcTools, ec)) {
        if (!entry.is_directory()) continue;
        auto ver = entry.path().filename().string();
        if (ver > latestVer) {
            latestVer = ver;
            latest = entry.path();
        }
    }
    return latest.empty() ? std::nullopt : std::optional{latest};
}

#endif // _WIN32

} // namespace

std::optional<std::filesystem::path> find_vs_install_path() {
#if defined(_WIN32)
    // Try strategies in order of reliability
    if (auto p = find_vs_via_vswhere()) return p;
    if (auto p = find_vs_via_env()) return p;
    if (auto p = find_vs_via_paths()) return p;
#endif
    return std::nullopt;
}

std::optional<std::filesystem::path> find_msvc_tools_dir() {
#if defined(_WIN32)
    auto vs = find_vs_install_path();
    if (!vs) return std::nullopt;
    return find_latest_msvc_tools(*vs);
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> find_std_module_source() {
#if defined(_WIN32)
    auto tools = find_msvc_tools_dir();
    if (!tools) return std::nullopt;

    auto stdIxx = *tools / "modules" / "std.ixx";
    if (std::filesystem::exists(stdIxx))
        return stdIxx;
#endif
    return std::nullopt;
}

std::optional<std::filesystem::path> find_cl() {
#if defined(_WIN32)
    auto tools = find_msvc_tools_dir();
    if (!tools) return std::nullopt;

    // cl.exe is at <tools>/bin/Hostx64/x64/cl.exe
    auto cl = *tools / "bin" / "Hostx64" / "x64" / "cl.exe";
    if (std::filesystem::exists(cl))
        return cl;
#endif
    return std::nullopt;
}

} // namespace mcpp::toolchain::msvc
