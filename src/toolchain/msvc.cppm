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
#include <cstdlib>

export module mcpp.toolchain.msvc;

import std;
import mcpp.platform;
import mcpp.toolchain.model;
import mcpp.toolchain.probe;
import mcpp.xlings;

export namespace mcpp::toolchain::msvc {

// Find a Visual Studio installation path (returns the newest found).
std::optional<std::filesystem::path> find_vs_install_path();

// Find the MSVC tools directory: <VS>/VC/Tools/MSVC/<latest_version>/
std::optional<std::filesystem::path> find_msvc_tools_dir();

// Find MSVC STL's std.ixx module source file.
std::optional<std::filesystem::path> find_std_module_source();

// Find cl.exe (for future MSVC toolchain support).
std::optional<std::filesystem::path> find_cl();

// ─── System-toolchain detection (msvc@system) ────────────────────────────
//
// mcpp treats MSVC as a *system* toolchain: it locates and identifies an
// installed Visual Studio / Build Tools, but never installs or removes one.

struct MsvcInstallation {
    std::filesystem::path vsRoot;        // …\Microsoft Visual Studio\2022\BuildTools
    std::string           vsProduct;     // "2022 BuildTools" (path-derived; may be empty)
    std::string           toolsVersion;  // "14.44.35207" (VC\Tools\MSVC\<dir>)
    std::filesystem::path clPath;        // …\bin\Hostx64\x64\cl.exe
    std::string           clVersion;     // "19.44.35211" (banner; empty if unparseable)
    std::string           arch;          // "x64" | "x86" | "arm64"
    bool                  hasStdModules = false; // modules\std.ixx present

    // Preferred user-facing version: compiler version, else tools version.
    std::string display_version() const {
        return clVersion.empty() ? toolsVersion : clVersion;
    }
};

// Locate the best (newest) usable installation. nullopt = MSVC absent.
std::optional<MsvcInstallation> detect_installation();

// Parse a cl.exe banner into (version, arch). Token-based so localized
// banners work: first "d.d.d[.d]" run is the version, arch is the arm64/x64/
// x86 token. Pure and cross-platform for unit testing.
std::optional<std::pair<std::string, std::string>>
parse_cl_banner(std::string_view banner);

// Map a cl banner arch token to the canonical windows-msvc triple.
std::string triple_for_arch(std::string_view arch);

// Multi-line guidance shown wherever MSVC is required but absent.
// States what was searched and how to install (mcpp does not install MSVC).
std::string install_guidance();

// Classify + enrich an already-probed cl.exe binary for detect():
// version/arch from the banner, targetTriple, driverIdent, std.ixx lookup,
// and the build env (INCLUDE/LIB/PATH from VC tools + Windows SDK) into
// tc.envOverrides. Missing SDK leaves envOverrides empty — detection still
// succeeds (selection UX must work on SDK-less boxes); the build path
// checks and errors with guidance.
std::expected<void, DetectError> enrich_toolchain_from_cl(Toolchain& tc);

// ─── Windows SDK + build environment (native cl.exe builds) ──────────────

struct WindowsSdk {
    std::filesystem::path root;      // C:\Program Files (x86)\Windows Kits\10
    std::string           version;   // "10.0.26100.0" (highest usable)
};

// Locate the Windows 10/11 SDK (highest version with ucrt headers).
std::optional<WindowsSdk> find_windows_sdk();

// Synthesize the environment cl.exe/link.exe need — what vcvars would set,
// derived directly from the located VC tools + SDK (no vcvarsall.bat run):
//   INCLUDE = <tools>\include; <sdk>\Include\<v>\{ucrt,um,shared,winrt}
//   LIB     = <tools>\lib\<arch>; <sdk>\Lib\<v>\{ucrt,um}\<arch>
//   PATH    = <cl dir>;<existing PATH>       (mspdb*.dll etc.)
//   VSLANG  = 1033  (stable English /showIncludes prefix for ninja deps=msvc)
std::vector<EnvVar> build_env_for_cl(const std::filesystem::path& clPath,
                                     std::string_view arch,
                                     const WindowsSdk& sdk);

// std / std.compat module staging commands (single cl step each):
//   cl /nologo <stdFlagAndDialect> /EHsc /W0 /O2 /c <tools>\modules\std.ixx
//      /ifcOutput <cacheDir>\ifc.cache\std.ifc /Fo:<cacheDir>\std.obj
std::vector<std::string> std_module_build_commands(
    const Toolchain& tc, const std::filesystem::path& cacheDir,
    std::string_view cppStandardFlag);
std::vector<std::string> std_compat_build_commands(
    const Toolchain& tc, const std::filesystem::path& cacheDir,
    std::string_view cppStandardFlag);

std::filesystem::path std_bmi_path(const std::filesystem::path& cacheDir);
std::filesystem::path staged_std_bmi_path(const std::filesystem::path& outputDir);
std::filesystem::path std_compat_bmi_path(const std::filesystem::path& cacheDir);
std::filesystem::path staged_std_compat_bmi_path(const std::filesystem::path& outputDir);

} // namespace mcpp::toolchain::msvc

namespace mcpp::toolchain::msvc {

namespace {

#if defined(_WIN32)

// Run a command and capture stdout (first line, trimmed).
std::string run_capture_line(const std::string& cmd) {
    auto r = mcpp::platform::process::capture(cmd);
    auto& out = r.output;
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
    // Newer VS installs use the major version as the directory ("18", seen
    // on windows-latest 2026-07: …\Microsoft Visual Studio\18\Enterprise),
    // older ones the year branding.
    static constexpr std::string_view years[] = {"19", "18", "2025", "2022", "2019", "2017"};
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

// ─── System-toolchain detection ──────────────────────────────────────────

std::optional<std::pair<std::string, std::string>>
parse_cl_banner(std::string_view banner) {
    // Version: first digit/dot run with at least two dots ("19.44.35211",
    // possibly four components). Never anchored to the English word
    // "Version" — localized banners reorder the sentence.
    std::string version;
    {
        std::string run;
        int dots = 0;
        auto flush = [&] {
            if (version.empty() && dots >= 2 && run.back() != '.')
                version = run;
            run.clear();
            dots = 0;
        };
        for (char c : banner) {
            if (c >= '0' && c <= '9') { run += c; }
            else if (c == '.' && !run.empty()) { run += c; ++dots; }
            else if (!run.empty()) { flush(); }
            if (!version.empty()) break;
        }
        if (!run.empty()) flush();
    }
    if (version.empty()) return std::nullopt;

    auto lower = mcpp::toolchain::lower_copy(banner);
    std::string arch;
    if (lower.find("arm64") != std::string::npos)     arch = "arm64";
    else if (lower.find("x64") != std::string::npos)  arch = "x64";
    else if (lower.find("x86") != std::string::npos)  arch = "x86";

    return std::pair{version, arch};
}

std::string triple_for_arch(std::string_view arch) {
    if (arch == "arm64") return "aarch64-pc-windows-msvc";
    if (arch == "x86")   return "i686-pc-windows-msvc";
    return "x86_64-pc-windows-msvc";
}

std::string install_guidance() {
    return
        "MSVC was not found on this system.\n"
        "  searched: vswhere.exe, VSINSTALLDIR / VS*COMNTOOLS, and the standard\n"
        "            'Program Files\\Microsoft Visual Studio\\<year>\\<edition>' paths\n"
        "  mcpp does not install MSVC — install it yourself, then retry:\n"
        "    - Visual Studio Installer: add the 'Desktop development with C++' workload\n"
        "      (component: Microsoft.VisualStudio.Component.VC.Tools.x86.x64)\n"
        "    - or Build Tools only: winget install Microsoft.VisualStudio.2022.BuildTools\n"
        "      then add the C++ workload in the installer\n"
        "  afterwards run: mcpp toolchain default msvc";
}

namespace {

// "…\Microsoft Visual Studio\2022\BuildTools" → "2022 BuildTools".
[[maybe_unused]] std::string product_from_vs_root(const std::filesystem::path& vsRoot) {
    std::vector<std::string> parts;
    // path iterators yield temporaries under libc++ — const ref only.
    for (const auto& seg : vsRoot) parts.push_back(seg.string());
    for (std::size_t i = 0; i + 2 < parts.size(); ++i) {
        if (parts[i] == "Microsoft Visual Studio")
            return parts[i + 1] + " " + parts[i + 2];
    }
    return {};
}

// Capture cl.exe's banner. cl prints it (plus a usage complaint) when run
// bare; the exit status is irrelevant — parse whatever came out.
std::string capture_cl_banner(const std::filesystem::path& cl) {
    auto r = mcpp::platform::process::capture(
        "\"" + cl.string() + "\" 2>&1");
    return r.output;
}

} // namespace

std::optional<MsvcInstallation> detect_installation() {
#if defined(_WIN32)
    auto vs = find_vs_install_path();
    if (!vs) return std::nullopt;
    auto tools = find_latest_msvc_tools(*vs);
    if (!tools) return std::nullopt;

    MsvcInstallation inst;
    inst.vsRoot       = *vs;
    inst.vsProduct    = product_from_vs_root(*vs);
    inst.toolsVersion = tools->filename().string();

    // Host-native bin dir first (arm64 hosts run arm64 cl; everything else
    // x64), with the remaining pairs as fallback.
    std::vector<std::pair<std::string_view, std::string_view>> pairs;
    if (mcpp::platform::host_arch == std::string_view("aarch64")
        || mcpp::platform::host_arch == std::string_view("arm64")) {
        pairs = {{"Hostarm64", "arm64"}, {"Hostx64", "x64"}, {"Hostx86", "x86"}};
    } else {
        pairs = {{"Hostx64", "x64"}, {"Hostarm64", "arm64"}, {"Hostx86", "x86"}};
    }
    for (auto [host, target] : pairs) {
        auto cl = *tools / "bin" / host / target / "cl.exe";
        std::error_code ec;
        if (std::filesystem::exists(cl, ec)) {
            inst.clPath = cl;
            inst.arch   = std::string(target);
            break;
        }
    }
    if (inst.clPath.empty()) return std::nullopt;

    std::error_code ec;
    inst.hasStdModules =
        std::filesystem::exists(*tools / "modules" / "std.ixx", ec);

    // Version identification: banner is authoritative; tolerate failure
    // (clVersion stays empty and display_version() falls back to the
    // tools-dir version).
    if (auto parsed = parse_cl_banner(capture_cl_banner(inst.clPath))) {
        inst.clVersion = parsed->first;
        if (!parsed->second.empty()) inst.arch = parsed->second;
    }
    return inst;
#else
    return std::nullopt;
#endif
}

std::optional<WindowsSdk> find_windows_sdk() {
#if defined(_WIN32)
    // Directory scan of the conventional install roots; highest version dir
    // that actually carries the UCRT headers wins. (Registry Installed
    // Roots would be marginally more correct — the path scan covers every
    // real installer layout seen so far and needs no Win32 API surface.)
    for (const char* base : {"C:\\Program Files (x86)\\Windows Kits\\10",
                             "C:\\Program Files\\Windows Kits\\10"}) {
        std::filesystem::path root{base};
        std::error_code ec;
        if (!std::filesystem::exists(root / "Include", ec)) continue;
        std::string best;
        for (auto& e : std::filesystem::directory_iterator(root / "Include", ec)) {
            if (!e.is_directory()) continue;
            auto v = e.path().filename().string();
            if (std::filesystem::exists(e.path() / "ucrt" / "corecrt.h", ec)
                && v > best)
                best = v;
        }
        if (!best.empty()) return WindowsSdk{root, best};
    }
#endif
    return std::nullopt;
}

std::vector<EnvVar> build_env_for_cl(const std::filesystem::path& clPath,
                                     std::string_view arch,
                                     const WindowsSdk& sdk) {
    // <tools>\bin\Host<h>\<arch>\cl.exe → <tools>
    auto clDir  = clPath.parent_path();
    auto tools  = clDir.parent_path().parent_path().parent_path();
    std::string a = arch.empty() ? std::string("x64") : std::string(arch);

    auto join = [](std::initializer_list<std::filesystem::path> ps) {
        std::string s;
        for (auto& p : ps) {
            if (!s.empty()) s += ';';
            s += p.string();
        }
        return s;
    };

    std::vector<EnvVar> env;
    env.push_back({"INCLUDE", join({
        tools / "include",
        sdk.root / "Include" / sdk.version / "ucrt",
        sdk.root / "Include" / sdk.version / "um",
        sdk.root / "Include" / sdk.version / "shared",
        sdk.root / "Include" / sdk.version / "winrt",
    })});
    env.push_back({"LIB", join({
        tools / "lib" / a,
        sdk.root / "Lib" / sdk.version / "ucrt" / a,
        sdk.root / "Lib" / sdk.version / "um" / a,
    })});
    std::string path = clDir.string();
    if (const char* p = std::getenv("PATH"); p && *p) {
        path += ';';
        path += p;
    }
    env.push_back({"PATH", std::move(path)});
    // Stable English "Note: including file:" prefix for ninja's deps=msvc.
    env.push_back({"VSLANG", "1033"});
    return env;
}

std::filesystem::path std_bmi_path(const std::filesystem::path& cacheDir) {
    return cacheDir / "ifc.cache" / "std.ifc";
}
std::filesystem::path staged_std_bmi_path(const std::filesystem::path& outputDir) {
    return outputDir / "ifc.cache" / "std.ifc";
}
std::filesystem::path std_compat_bmi_path(const std::filesystem::path& cacheDir) {
    return cacheDir / "ifc.cache" / "std.compat.ifc";
}
std::filesystem::path staged_std_compat_bmi_path(const std::filesystem::path& outputDir) {
    return outputDir / "ifc.cache" / "std.compat.ifc";
}

namespace {

std::string cl_stage_command(const Toolchain& tc,
                             const std::filesystem::path& cacheDir,
                             std::string_view cppStandardFlag,
                             const std::filesystem::path& source,
                             const std::filesystem::path& ifcOut,
                             std::string_view objName,
                             std::string_view extraRef) {
    // cd into the cache dir (relative outputs land there); env (INCLUDE/LIB)
    // comes from tc.envOverrides via the executor, not the command string.
    // `/d`: cmd.exe won't change DRIVE without it (workspace on D:, BMI
    // cache on C: is the real CI layout).
    return std::format(
        "cd /d {} && {} /nologo {} /EHsc /O2 /W0{} /c {} /ifcOutput {} /Fo:{} 2>&1",
        mcpp::xlings::shq(cacheDir.string()),
        mcpp::xlings::shq(tc.binaryPath.string()),
        cppStandardFlag,
        extraRef,
        mcpp::xlings::shq(source.string()),
        mcpp::xlings::shq(ifcOut.string()),
        objName);
}

} // namespace

std::vector<std::string> std_module_build_commands(
    const Toolchain& tc, const std::filesystem::path& cacheDir,
    std::string_view cppStandardFlag) {
    return { cl_stage_command(tc, cacheDir, cppStandardFlag,
                              tc.stdModuleSource,
                              std_bmi_path(cacheDir), "std.obj", "") };
}

std::vector<std::string> std_compat_build_commands(
    const Toolchain& tc, const std::filesystem::path& cacheDir,
    std::string_view cppStandardFlag) {
    // std.compat imports std — reference the freshly staged std.ifc.
    auto ref = std::format(" /reference std={}",
                           mcpp::xlings::shq(std_bmi_path(cacheDir).string()));
    return { cl_stage_command(tc, cacheDir, cppStandardFlag,
                              tc.stdCompatSource,
                              std_compat_bmi_path(cacheDir), "std.compat.obj",
                              ref) };
}

std::expected<void, DetectError> enrich_toolchain_from_cl(Toolchain& tc) {
    auto banner = capture_cl_banner(tc.binaryPath);
    auto parsed = parse_cl_banner(banner);
    if (!parsed) {
        return std::unexpected(DetectError{std::format(
            "'{}' looks like MSVC cl but produced no recognizable banner:\n{}",
            tc.binaryPath.string(), banner)});
    }
    tc.compiler     = CompilerId::MSVC;
    tc.version      = parsed->first;
    tc.targetTriple = triple_for_arch(parsed->second);
    tc.driverIdent  = mcpp::toolchain::normalize_driver_output(banner);

    // MSVC STL ships std.ixx next to the tools dir the cl binary lives in:
    // <tools>\bin\Host*\*\cl.exe → <tools>\modules\std.ixx.
    auto toolsDir = tc.binaryPath.parent_path()   // x64
                        .parent_path()            // Hostx64
                        .parent_path()            // bin
                        .parent_path();           // <tools>
    std::error_code ec;
    if (auto ixx = toolsDir / "modules" / "std.ixx";
        std::filesystem::exists(ixx, ec)) {
        tc.stdModuleSource = ixx;
        tc.hasImportStd    = true;
    } else if (auto found = find_std_module_source()) {
        tc.stdModuleSource = *found;
        tc.hasImportStd    = true;
    }
    if (auto compat = toolsDir / "modules" / "std.compat.ixx";
        std::filesystem::exists(compat, ec)) {
        tc.stdCompatSource = compat;
    }

    // Build environment (INCLUDE/LIB/PATH/VSLANG). SDK absence keeps
    // detection working (selection UX on SDK-less boxes); the build path
    // errors with guidance when envOverrides is empty.
    if (auto sdk = find_windows_sdk()) {
        tc.envOverrides = build_env_for_cl(tc.binaryPath, parsed->second, *sdk);
    }
    return {};
}

} // namespace mcpp::toolchain::msvc
