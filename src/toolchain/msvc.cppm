// mcpp.toolchain.msvc — locating an MSVC toolset, from either origin.
//
// A toolset reaches a build one of two ways, and they answer different
// questions:
//
//   SYSTEM  (`msvc@system`)      — probed on this machine. The answer depends
//                                  on what happens to be installed here.
//   MANAGED (`msvc@<toolset>`)   — an xlings payload the manifest named. The
//                                  answer is in the manifest; the machine only
//                                  decides whether it has been downloaded yet.
//
// Everything below is one of those two, or shared between them. The shared
// part is `installation_from_tools_dir()`: given a `VC/Tools/MSVC/<ver>`
// directory, the record built from it is identical whichever origin produced
// it — which is what keeps a managed toolset from being a second code path
// with its own bugs.
//
// Discovery order for the SYSTEM origin is deliberate and is documented at
// find_vs_install_path(): a declared answer outranks a probe.
//
// Also used by clang.cppm to find MSVC STL's std.ixx when Clang targets
// x86_64-pc-windows-msvc.

module;
#include <cstdlib>

export module mcpp.toolchain.msvc;

import std;
import mcpp.platform;
import mcpp.toolchain.model;
import mcpp.toolchain.probe;
import mcpp.platform.xlings;

export namespace mcpp::toolchain::msvc {

// Find a Visual Studio installation path (returns the newest found).
std::optional<std::filesystem::path> find_vs_install_path();

// Find the MSVC tools directory: <VS>/VC/Tools/MSVC/<latest_version>/
std::optional<std::filesystem::path> find_msvc_tools_dir();

// Find MSVC STL's std.ixx module source file.
std::optional<std::filesystem::path> find_std_module_source();

// Find cl.exe (for future MSVC toolchain support).
std::optional<std::filesystem::path> find_cl();

// Lowest -std= level MSVC STL builds the `std` module at, for a toolchain
// whose `version` is a cl banner version ("19.44.35211").
//
// microsoft/STL#3945 ("Supporting `import std;` in C++20") was fixed by
// STL#3977 (merged 2023-08-31) — the C++20 block was a policy choice with no
// technical reason behind it. That first ships in VS 2022 17.8, i.e. cl 19.38;
// older STLs still refuse and would fail inside std.ixx, so they answer 23 and
// get an actionable diagnostic from the caller instead. This is also what keeps
// the level gate reachable: every other provider answers 20.
int std_module_min_level(const Toolchain& tc);

// ─── Installation records (both origins) ─────────────────────────────────

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

// SYSTEM origin: locate the best (newest) usable installation on this
// machine. nullopt = MSVC absent. Everything about which toolset this picks
// is a property of the machine, not of the caller — see `installation_at`
// for the other origin.
std::optional<MsvcInstallation> detect_installation();

// MANAGED origin: build the record for an EXACT toolset under a VS-shaped
// root (`<vsRoot>/VC/Tools/MSVC/<toolsVersion>`).
//
// Nothing is probed and nothing is ranked: `toolsVersion` is what the caller
// declared, so a missing directory is nullopt rather than a silent fallback
// to a neighbouring toolset. That is the whole difference from
// detect_installation(), and it is why a manifest pinning a toolset gets the
// same compiler on every machine.
//
// Not Windows-only: given a directory of that shape the record is the same
// anywhere, which is what makes the managed path testable off Windows. The
// cl banner simply stays unparsed there and `display_version()` falls back
// to the declared version.
std::optional<MsvcInstallation> installation_at(const std::filesystem::path& vsRoot,
                                                std::string_view toolsVersion);

// Parse a cl.exe banner into (version, arch). Token-based so localized
// banners work: first "d.d.d[.d]" run is the version, arch is the arm64/x64/
// x86 token. Pure and cross-platform for unit testing.
std::optional<std::pair<std::string, std::string>>
parse_cl_banner(std::string_view banner);

// Map a cl banner arch token to the canonical windows-msvc triple.
std::string triple_for_arch(std::string_view arch);

// Multi-line guidance shown wherever MSVC is required but absent: what was
// searched, and both ways to get a compiler (pin one, or use the machine's).
std::string install_guidance();

// A version-axis spelling that no longer means what it used to.
//
// `msvc@19.44` was a pin-verify against the SYSTEM install's cl banner. The
// version axis now names a toolset (`14.44.35207`), so that spelling has to
// say so — and say what the two things it might have meant are spelled as.
//
// Returns guidance only when this machine can PROVE that reading (its own cl
// banner matches the requested prefix), which makes the message a fact about
// this machine rather than a guess about a string. nullopt otherwise, so an
// ordinary "no such toolset" error is not decorated with speculation.
std::optional<std::string> cl_version_spelling_hint(std::string_view requestedVersion);

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

// Locate the Windows 10/11 SDK. Search order, most specific first:
//
//   1. WindowsSdkDir (+ WindowsSdkVersion) — what vcvars exports and what
//      every other build system honours. A declared answer outranks a scan,
//      for the same reason VSINSTALLDIR outranks vswhere.
//   2. `extraRoots` — roots the caller already knows about. In practice the
//      windows-sdk payloads sitting beside a managed toolset in mcpp's own
//      store; see sibling_sdk_roots().
//   3. The conventional absolute install roots — a fallback, not the rule.
//      They are still here because a system Visual Studio does put the SDK
//      there, and nothing else would find it.
//
// Within a root the highest version carrying `ucrt/corecrt.h` wins, unless
// WindowsSdkVersion named one that is present.
std::optional<WindowsSdk> find_windows_sdk(
    std::span<const std::filesystem::path> extraRoots = {});

// Windows SDK payload roots that belong to the same xlings store as this
// compiler. The compiler binary says which store it came from, so a managed
// toolset finds its own SDK with nothing configured and no version hardcoded
// anywhere in mcpp. Empty for a system cl.exe (it is not in a store).
std::vector<std::filesystem::path>
sibling_sdk_roots(const std::filesystem::path& clPath);

// True only when BOTH halves of a usable MSVC C++ setup are present: the
// STL's std module source AND the Windows SDK.
//
// Either half alone is a half-installed state — Visual Studio with only the
// .NET workload, or VC tools without the SDK — that a cheaper
// `find_vs_install_path()` probe would happily call "MSVC is here", only for
// the build to fail later inside the compiler. Selecting a toolchain on a
// weaker signal than the one the build actually needs is the bug this
// predicate exists to prevent, so it deliberately asks for both.
//
// Always false off Windows: the whole discovery chain is Win32-only.
bool has_usable_msvc();

// Whether MSVC is usable HERE — either origin. `has_usable_msvc()` asks only
// about the machine, which is the wrong question wherever a managed toolset
// would serve just as well: a box with a pinned `msvc@14.44.35207` payload
// and no Visual Studio answers `false` to that one while being perfectly able
// to compile.
//
// `pkgsDir` is mcpp's payload store; a non-empty `xim-x-msvc/<ver>` with a
// resolvable cl.exe counts. Off Windows both are false — the whole chain is
// Win32-only.
bool msvc_available_here(const std::filesystem::path& pkgsDir);

// The redistributable VC runtime that BELONGS TO THIS TOOLSET:
//   <VC>\Redist\MSVC\<redistVer>\<arch>\Microsoft.VC<N>.CRT\
//     vcruntime140.dll  msvcp140.dll  ...
//
// It matters because the default CRT model is /MD, and those DLLs are NOT
// Windows components — ucrtbase.dll ships with the OS, vcruntime140.dll does
// not. On a machine that has only a managed toolset (no Visual Studio, no
// redistributable installed) a /MD build links fine and then cannot start.
//
// The toolset carries its own copy, so this is the toolchain-coupled runtime
// in exactly the sense libstdc++ is for gcc, and it goes in the same field.
//
// `redistVer` is NOT the tools version (14.44.35112 vs 14.44.35207), so the
// newest directory is chosen rather than derived. `debug_nonredist\` is never
// returned: those DLLs may not be redistributed.
std::filesystem::path vc_redist_dir(const std::filesystem::path& clPath,
                                    std::string_view arch = "x64");

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
// `crtFlag` is the CRT model (`/MT` or `/MD`) the PROJECT'S TUs are compiled
// with. It has to be handed in rather than defaulted, for the same reason
// `macos_deployment_target` is: cl bakes `_MSVC_MT` / `_MSVC_MD` into the
// module, and a TU importing a std built with the other one gets C5050 followed
// by a real C2375 out of the ucrt headers. Empty keeps cl's own default.
std::vector<std::string> std_module_build_commands(
    const Toolchain& tc, const std::filesystem::path& cacheDir,
    std::string_view cppStandardFlag, std::string_view crtFlag = {});
std::vector<std::string> std_compat_build_commands(
    const Toolchain& tc, const std::filesystem::path& cacheDir,
    std::string_view cppStandardFlag, std::string_view crtFlag = {});

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

// Strategy 1: VSINSTALLDIR — someone SAID which install to use.
//
// Set by a developer command prompt, by a CI step that ran vcvarsall, or by
// a tool that exported an environment on purpose. It is an answer, not a
// guess, which is why it now outranks vswhere.
std::optional<std::filesystem::path> find_vs_via_vsinstalldir() {
    if (auto* dir = std::getenv("VSINSTALLDIR"); dir && *dir) {
        std::filesystem::path p{dir};
        if (std::filesystem::exists(p / "VC" / "Tools" / "MSVC"))
            return p;
    }
    return std::nullopt;
}

// Strategy 2: vswhere.exe — Microsoft's locator, i.e. a ranked guess.
std::optional<std::filesystem::path> find_vs_via_vswhere() {
    // vswhere.exe ships with the VS Installer at a well-known path
    std::filesystem::path vswhere =
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (!std::filesystem::exists(vswhere)) return std::nullopt;

    // `-prerelease` or an Insiders instance is invisible here. Without it a
    // machine with only an Insiders VS reports "MSVC was not found" while a
    // perfectly good cl.exe sits on disk.
    auto result = run_capture_line(
        "\"" + vswhere.string() + "\" -latest -prerelease -products * "
        "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
        "-property installationPath 2>nul");

    if (!result.empty() && std::filesystem::exists(result))
        return std::filesystem::path(result);
    return std::nullopt;
}

// Strategy 3: VS*COMNTOOLS — machine-wide leftovers, so they rank BELOW
// vswhere. VS150COMNTOOLS lingering from a 2017 install must not outrank a
// current one; unlike VSINSTALLDIR nobody set these for this shell.
std::optional<std::filesystem::path> find_vs_via_comntools() {
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

// Strategy 4: Scan well-known paths.
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
    // Declared before probed. vswhere used to run first, and because it
    // returns something on almost every developer machine, VSINSTALLDIR was
    // effectively unreachable — a build that had exported a complete vcvars
    // environment still compiled with whatever vswhere ranked highest
    // (measured on xrgui#3: vcvars said 14.52, the build used 14.51, and the
    // only way out was to hide vswhere.exe). A guess must not silently
    // override an answer.
    if (auto p = find_vs_via_vsinstalldir()) return p;
    if (auto p = find_vs_via_vswhere())      return p;
    if (auto p = find_vs_via_comntools())    return p;
    if (auto p = find_vs_via_paths())        return p;
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
        "no Visual Studio installation was found on this system.\n"
        "  searched: VSINSTALLDIR, vswhere.exe, VS*COMNTOOLS, and the standard\n"
        "            'Program Files\\Microsoft Visual Studio\\<year>\\<edition>' paths\n"
        "\n"
        "  mcpp can install a pinned MSVC toolset instead — no Visual Studio,\n"
        "  no installer, no elevation, and several toolsets can coexist:\n"
        "    mcpp toolchain install msvc 14.44.35207\n"
        "    mcpp toolchain list --available msvc     # other toolsets\n"
        "  then pin it in mcpp.toml, so every machine builds with that one:\n"
        "    [toolchain]\n"
        "    windows = \"msvc@14.44.35207\"\n"
        "\n"
        "  or install Visual Studio yourself and use msvc@system:\n"
        "    - Visual Studio Installer: add the 'Desktop development with C++' workload\n"
        "      (component: Microsoft.VisualStudio.Component.VC.Tools.x86.x64)\n"
        "    - or Build Tools only: winget install Microsoft.VisualStudio.2022.BuildTools\n"
        "      then add the C++ workload in the installer\n"
        "    afterwards run: mcpp toolchain default msvc";
}

std::optional<std::string> cl_version_spelling_hint(std::string_view requestedVersion) {
    if (requestedVersion.empty() || requestedVersion == "system")
        return std::nullopt;
    auto inst = detect_installation();
    if (!inst) return std::nullopt;
    // Only when the requested string really is this machine's cl version.
    // A toolset version ("14.44.35207") never prefixes a banner ("19.44.…"),
    // so this cannot fire on a genuine typo'd toolset.
    if (!inst->display_version().starts_with(requestedVersion)) return std::nullopt;
    if (inst->display_version() == inst->toolsVersion) return std::nullopt;  // banner unparsed
    return std::format(
        "msvc@{} names a COMPILER version, not a toolset.\n"
        "  This machine's Visual Studio reports cl {} (VC tools {}).\n"
        "  The version axis now selects the toolset mcpp installs and pins:\n"
        "    - to use this machine's install:  msvc@system\n"
        "    - to pin a toolset mcpp manages:  msvc@{}",
        requestedVersion, inst->display_version(), inst->toolsVersion,
        inst->toolsVersion);
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

// The one place an MsvcInstallation is built — from a VS root and a
// `VC/Tools/MSVC/<ver>` directory under it. Both origins land here, so a
// managed toolset and a system one are described by the same code and cannot
// drift apart in what they report or which cl.exe they pick.
//
// Deliberately not Windows-guarded: given a directory of that shape the
// record is the same anywhere, which is what makes the managed path testable
// on a Linux CI runner.
std::optional<MsvcInstallation>
installation_from_tools_dir(const std::filesystem::path& vsRoot,
                            const std::filesystem::path& tools);

// Capture cl.exe's banner. cl prints it (plus a usage complaint) when run
// bare; the exit status is irrelevant — parse whatever came out.
std::string capture_cl_banner(const std::filesystem::path& cl) {
    auto r = mcpp::platform::process::capture(
        "\"" + cl.string() + "\" 2>&1");
    return r.output;
}

std::optional<MsvcInstallation>
installation_from_tools_dir(const std::filesystem::path& vsRoot,
                            const std::filesystem::path& tools) {
    MsvcInstallation inst;
    inst.vsRoot       = vsRoot;
    inst.vsProduct    = product_from_vs_root(vsRoot);
    inst.toolsVersion = tools.filename().string();

    // Host-native bin dir first (arm64 hosts run arm64 cl; everything else
    // x64), with the remaining pairs as fallback.
    std::vector<std::pair<std::string_view, std::string_view>> pairs;
    if (mcpp::platform::host_arch == std::string_view("aarch64")
        || mcpp::platform::host_arch == std::string_view("arm64")) {
        pairs = {{"Hostarm64", "arm64"}, {"Hostx64", "x64"}, {"Hostx86", "x86"}};
    } else {
        pairs = {{"Hostx64", "x64"}, {"Hostarm64", "arm64"}, {"Hostx86", "x86"}};
    }
    std::error_code ec;
    for (auto [host, target] : pairs) {
        auto cl = tools / "bin" / host / target / "cl.exe";
        if (std::filesystem::exists(cl, ec)) {
            inst.clPath = cl;
            inst.arch   = std::string(target);
            break;
        }
    }
    if (inst.clPath.empty()) return std::nullopt;

    inst.hasStdModules =
        std::filesystem::exists(tools / "modules" / "std.ixx", ec);

    // Version identification: banner is authoritative; tolerate failure
    // (clVersion stays empty and display_version() falls back to the
    // tools-dir version). Off Windows that failure is the normal case.
    if (auto parsed = parse_cl_banner(capture_cl_banner(inst.clPath))) {
        inst.clVersion = parsed->first;
        if (!parsed->second.empty()) inst.arch = parsed->second;
    }
    return inst;
}

} // namespace

std::optional<MsvcInstallation> installation_at(const std::filesystem::path& vsRoot,
                                                std::string_view toolsVersion) {
    if (toolsVersion.empty()) return std::nullopt;
    auto tools = vsRoot / "VC" / "Tools" / "MSVC" / std::string(toolsVersion);
    std::error_code ec;
    if (!std::filesystem::is_directory(tools, ec)) return std::nullopt;
    return installation_from_tools_dir(vsRoot, tools);
}

std::optional<MsvcInstallation> detect_installation() {
#if defined(_WIN32)
    auto vs = find_vs_install_path();
    if (!vs) return std::nullopt;
    auto tools = find_latest_msvc_tools(*vs);
    if (!tools) return std::nullopt;
    return installation_from_tools_dir(*vs, *tools);
#else
    return std::nullopt;
#endif
}

std::vector<std::filesystem::path>
sibling_sdk_roots(const std::filesystem::path& clPath) {
    std::vector<std::filesystem::path> out;
    auto xpkgs = mcpp::xlings::paths::xpkgs_from_compiler(clPath);
    if (!xpkgs) return out;   // a system cl.exe: not in any store
    std::error_code ec;
    auto pkgRoot = *xpkgs / "xim-x-windows-sdk";
    for (auto& e : std::filesystem::directory_iterator(pkgRoot, ec)) {
        if (e.is_directory(ec)) out.push_back(e.path());
    }
    // Newest payload version first, so the "highest usable" rule inside
    // find_windows_sdk() sees them in the order it would have picked anyway.
    std::sort(out.begin(), out.end(), std::greater<>{});
    return out;
}

// The `Lib\<v>\um\<arch>` subdirectory whose absence makes a root unusable
// FOR THIS HOST. Spelled from the host architecture rather than the build's
// target: a cross-compiling link is the caller's business (`build_env_for_cl`
// takes an arch), but a root with no host-arch libs at all is not an SDK this
// machine can link against. The names are the SDK's own.
#if defined(_M_ARM64) || defined(__aarch64__)
constexpr std::string_view sdk_lib_arch = "arm64";
#else
constexpr std::string_view sdk_lib_arch = "x64";
#endif

std::optional<WindowsSdk> find_windows_sdk(
    std::span<const std::filesystem::path> extraRoots) {
    // Highest version dir under `root/Include` that actually carries the UCRT
    // headers; `want` (from WindowsSdkVersion) wins if it is one of them.
    // (Registry Installed Roots would be marginally more correct — the path
    // scan covers every real installer layout seen so far and needs no Win32
    // API surface.)
    // BOTH halves, for the same reason `has_usable_msvc()` asks for both: an
    // SDK is headers AND import libraries, and a root carrying only the first
    // is a half-installed state that this function used to call "found".
    //
    // That is not hypothetical. A managed `xim:windows-sdk` payload whose
    // ucrt MSI had unpacked but whose um-libs MSI had not left
    // `Include/<v>/ucrt/corecrt.h` on disk with no `kernel32.lib` anywhere;
    // the header check passed, the root was selected over the machine's own
    // complete SDK, every translation unit compiled, and the build died at
    //     LINK : fatal error LNK1104: cannot open file 'kernel32.lib'
    // with nothing in the log naming the SDK. Rejecting the partial root
    // makes the search fall through to the next one, which is the behaviour
    // a user would expect from a probe that reports "not found".
    //
    // kernel32.lib is the right sentinel: every link needs it, and unlike the
    // ucrt libs it is not spread across the SDK's optional pieces.
    auto pick = [](const std::filesystem::path& root,
                   std::string_view want) -> std::optional<WindowsSdk> {
        std::error_code ec;
        auto inc = root / "Include";
        if (!std::filesystem::is_directory(inc, ec)) return std::nullopt;
        auto usable = [&](const std::filesystem::path& verDir,
                          const std::string& v) {
            return std::filesystem::exists(verDir / "ucrt" / "corecrt.h", ec)
                && std::filesystem::exists(
                       root / "Lib" / v / "um" / sdk_lib_arch / "kernel32.lib", ec);
        };
        std::string best;
        for (auto& e : std::filesystem::directory_iterator(inc, ec)) {
            if (!e.is_directory(ec)) continue;
            auto v = e.path().filename().string();
            if (!usable(e.path(), v)) continue;
            if (!want.empty() && v == want) return WindowsSdk{root, v};
            if (v > best) best = v;
        }
        if (best.empty()) return std::nullopt;
        return WindowsSdk{root, best};
    };

    // 1. Declared: WindowsSdkDir (+ WindowsSdkVersion). vcvars exports both;
    //    WindowsSdkVersion carries a trailing backslash there, which is not
    //    part of the directory name.
    std::string want;
    if (auto* v = std::getenv("WindowsSdkVersion"); v && *v) {
        want = v;
        while (!want.empty() && (want.back() == '\\' || want.back() == '/'))
            want.pop_back();
    }
    if (auto* dir = std::getenv("WindowsSdkDir"); dir && *dir) {
        if (auto s = pick(std::filesystem::path{dir}, want)) return s;
    }

    // 2. Roots the caller knows about (managed toolset's own store).
    for (const auto& root : extraRoots)
        if (auto s = pick(root, want)) return s;

    // 3. The conventional absolute install roots.
    for (const char* base : {"C:\\Program Files (x86)\\Windows Kits\\10",
                             "C:\\Program Files\\Windows Kits\\10"}) {
        if (auto s = pick(std::filesystem::path{base}, want)) return s;
    }
    return std::nullopt;
}

bool has_usable_msvc() {
#if defined(_WIN32)
    // Both, deliberately — see the declaration for why either half alone is
    // a trap. Order matters only for cost: the STL probe short-circuits the
    // SDK directory scan on machines with no Visual Studio at all.
    return find_std_module_source().has_value() && find_windows_sdk().has_value();
#else
    return false;
#endif
}

bool msvc_available_here([[maybe_unused]] const std::filesystem::path& pkgsDir) {
#if defined(_WIN32)
    if (has_usable_msvc()) return true;
    // A managed toolset is just as usable, and asking the machine about it
    // gets the wrong answer. Any installed version whose cl.exe resolves
    // counts; `installation_at` is the same resolution install and build use.
    std::error_code ec;
    auto root = pkgsDir / "xim-x-msvc";
    if (!std::filesystem::is_directory(root, ec)) return false;
    for (auto& v : std::filesystem::directory_iterator(root, ec)) {
        if (!v.is_directory(ec)) continue;
        if (installation_at(v.path(), v.path().filename().string())) return true;
    }
    return false;
#else
    return false;
#endif
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
                             std::string_view extraRef,
                             std::string_view crtFlag) {
    // cd into the cache dir (relative outputs land there); env (INCLUDE/LIB)
    // comes from tc.envOverrides via the executor, not the command string.
    // `/d`: cmd.exe won't change DRIVE without it (workspace on D:, BMI
    // cache on C: is the real CI layout).
    return std::format(
        "cd /d {} && {} /nologo {}{} /EHsc /O2 /W0{} /c {} /ifcOutput {} /Fo:{} 2>&1",
        mcpp::xlings::shq(cacheDir.string()),
        mcpp::xlings::shq(tc.binaryPath.string()),
        cppStandardFlag,
        crtFlag.empty() ? std::string{} : std::format(" {}", crtFlag),
        extraRef,
        mcpp::xlings::shq(source.string()),
        mcpp::xlings::shq(ifcOut.string()),
        objName);
}

} // namespace

int std_module_min_level(const Toolchain& tc) {
    // Two-segment compare: cppfly::compiler_major only reads the leading
    // integer, which is 19 for every MSVC ever shipped. Keep that function's
    // meaning intact (it has other consumers) and do the version knowledge
    // here, where the rest of the cl banner handling already lives.
    int major = 0, minor = 0;
    auto it = tc.version.begin();
    const auto end = tc.version.end();
    auto read = [&](int& out) {
        bool any = false;
        while (it != end && *it >= '0' && *it <= '9') {
            out = out * 10 + (*it - '0');
            ++it;
            any = true;
        }
        return any;
    };
    if (!read(major)) return 23;            // unparseable banner: stay strict
    if (it != end && *it == '.') ++it;
    read(minor);
    const bool atLeast_19_38 = major > 19 || (major == 19 && minor >= 38);
    return atLeast_19_38 ? 20 : 23;
}

std::vector<std::string> std_module_build_commands(
    const Toolchain& tc, const std::filesystem::path& cacheDir,
    std::string_view cppStandardFlag, std::string_view crtFlag) {
    return { cl_stage_command(tc, cacheDir, cppStandardFlag,
                              tc.stdModuleSource,
                              std_bmi_path(cacheDir), "std.obj", "", crtFlag) };
}

std::vector<std::string> std_compat_build_commands(
    const Toolchain& tc, const std::filesystem::path& cacheDir,
    std::string_view cppStandardFlag, std::string_view crtFlag) {
    // std.compat imports std — reference the freshly staged std.ifc.
    auto ref = std::format(" /reference std={}",
                           mcpp::xlings::shq(std_bmi_path(cacheDir).string()));
    return { cl_stage_command(tc, cacheDir, cppStandardFlag,
                              tc.stdCompatSource,
                              std_compat_bmi_path(cacheDir), "std.compat.obj",
                              ref, crtFlag) };
}

std::filesystem::path vc_redist_dir(const std::filesystem::path& clPath,
                                    std::string_view arch) {
    // <VC>/Tools/MSVC/<ver>/bin/Host<h>/<arch>/cl.exe → up 6 from the arch dir
    auto vc = clPath.parent_path();
    for (int i = 0; i < 6 && !vc.empty(); ++i) vc = vc.parent_path();
    std::error_code ec;
    auto redist = vc / "Redist" / "MSVC";
    if (!std::filesystem::is_directory(redist, ec)) return {};

    std::filesystem::path best;
    std::string bestVer;
    for (auto& v : std::filesystem::directory_iterator(redist, ec)) {
        if (!v.is_directory(ec)) continue;
        auto archDir = v.path() / std::string(arch);
        if (!std::filesystem::is_directory(archDir, ec)) continue;
        for (auto& c : std::filesystem::directory_iterator(archDir, ec)) {
            if (!c.is_directory(ec)) continue;
            auto name = c.path().filename().string();
            // Microsoft.VC143.CRT — the CRT, not CXXAMP/OpenMP, and never
            // anything under debug_nonredist (which is not redistributable
            // and is a sibling of <arch>, not a child, but be explicit).
            if (!name.starts_with("Microsoft.VC") || !name.ends_with(".CRT"))
                continue;
            if (c.path().string().find("debug_nonredist") != std::string::npos)
                continue;
            if (auto ver = v.path().filename().string(); ver > bestVer) {
                bestVer = ver;
                best = c.path();
            }
        }
    }
    return best;
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
    if (tc.hasImportStd) {
        tc.importStdMinLevel = std_module_min_level(tc);
    }
    if (auto compat = toolsDir / "modules" / "std.compat.ixx";
        std::filesystem::exists(compat, ec)) {
        tc.stdCompatSource = compat;
    }

    // Build environment (INCLUDE/LIB/PATH/VSLANG). SDK absence keeps
    // detection working (selection UX on SDK-less boxes); the build path
    // errors with guidance when envOverrides is empty.
    //
    // A managed toolset carries its own SDK as an xlings dependency, and the
    // compiler's own path says which store to look in — so the two halves of
    // a pinned toolchain stay together without anything being configured.
    auto extraSdkRoots = sibling_sdk_roots(tc.binaryPath);
    if (auto sdk = find_windows_sdk(extraSdkRoots)) {
        tc.envOverrides = build_env_for_cl(tc.binaryPath, parsed->second, *sdk);
    }

    // The toolset's own redistributable CRT, in the same field gcc uses for
    // libstdc++ — so `mcpp run` puts it on PATH exactly the way it puts a
    // private libstdc++ on LD_LIBRARY_PATH.
    //
    // Without it, the DEFAULT build (/MD) links vcruntime140.dll and
    // msvcp140.dll, which are not OS components, and a machine with only a
    // managed toolset cannot start what it just built. That machine is not
    // hypothetical — it is any box that installed `msvc@<toolset>` and never
    // had Visual Studio. CI does not see it because the runners have VS.
    //
    // Safe for the link line: hostflags returns early for MSVC before it
    // emits -L, and flags.cppm's runtime-dir block is `supports_rpath`-gated,
    // so nothing here reaches cl or link as a flag.
    if (auto redist = vc_redist_dir(tc.binaryPath, parsed->second);
        !redist.empty()) {
        tc.linkRuntimeDirs.push_back(redist);
    }
    return {};
}

} // namespace mcpp::toolchain::msvc
