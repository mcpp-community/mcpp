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
// Which installed toolset the SYSTEM origin means, and which one a pinned
// `msvc@<version>` finds before it reaches for a payload, is one function,
// `select_system_toolset()`: a declared answer outranks a probe, and the cl.exe
// row and the clang row read the same answer.
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

// Lowest -std= level MSVC STL builds the `std` module at.
//
// microsoft/STL#3945 ("Supporting `import std;` in C++20") was fixed by
// STL#3977 (merged 2023-08-31) — the C++20 block was a policy choice with no
// technical reason behind it. That first ships in VS 2022 17.8, i.e. cl 19.38
// and toolset 14.38; older STLs still refuse and would fail inside std.ixx, so
// they answer 23 and get an actionable diagnostic from the caller instead. This
// is also what keeps the level gate reachable: every other provider answers 20.
//
// ASKED OF THE STL, NOT OF THE COMPILER, and that distinction is the whole
// reason this takes a path.
//
// The question is a property of the standard library being compiled, and two
// different compilers reach the same `std.ixx`: cl.exe under
// `windows = "msvc@system"`, and clang targeting `*-windows-msvc` when no
// libc++ std module is present. The clang path used to hardcode 23 with a
// comment saying why it could not ask -- "tc.version is clang's here, so it
// cannot answer the cl-banner question" -- which is correct about the field
// and is an argument for changing the input rather than for assuming the
// worst. Calling the banner form from there would compare a CLANG version
// number against an MSVC threshold: clang 20.x would pass it by accident and
// clang 19.x would fail it wrongly, both by asking the wrong object.
//
// The toolset version is in the path of the module source that was already
// selected --
//
//     <VS>/VC/Tools/MSVC/14.44.35207/modules/std.ixx
//
// -- and toolset `14.<N>` pairs with cl banner `19.<N>`, so the existing
// `>= 38` predicate transfers unchanged. Taking it from the SELECTED file
// rather than from a fresh search matters on a machine with two installations:
// the answer must describe the STL that will actually be compiled.
//
// A path with no parseable `14.<minor>` component answers 23, which keeps the
// safety the hardcode was after without charging every modern installation for
// it.
int std_module_min_level_for_stl(const std::filesystem::path& stdModuleSource);

// The cl-banner form, kept for a toolchain whose `version` is a cl banner
// version ("19.44.35211") and no module source has been located yet.
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
// `identifyVersion = false` skips running cl.exe for its banner. Use it when
// the question is only "is there a usable toolset here" -- `msvc_available_here`
// asks that on every build, and spawning a compiler per installed payload to
// answer it is a latency regression on exactly the machines that have several.
std::optional<MsvcInstallation> installation_at(const std::filesystem::path& vsRoot,
                                                std::string_view toolsVersion,
                                                bool identifyVersion = true);

// ─── Toolset selection: one rule for both rows ────────────────────────────
//
// A toolset is chosen for two different rows. `cl.exe` compiles with it, and
// clang targeting `*-windows-msvc` compiles AGAINST it (its STL, CRT and the
// SDK that follows it). Both rows used to reach an answer on their own: the
// clang driver probed the machine for headers and libraries while this module
// probed it again for `std.ixx`, by a different order, so the two could name
// different toolsets. The selection below is the one answer both rows read.
//
// It is a function of its inputs. The machine enters only through
// `enumerate_vs_instances()` and `msvc_env_snapshot()`, which are the Windows
// half; everything else reads the directories it is handed, so it is tested
// on any host against a synthetic tree.

// One Visual Studio instance, as the installer reports it.
struct VsInstance {
    std::filesystem::path root;            // ...\Microsoft Visual Studio\2022\Community
    std::string           product;         // "Visual Studio Community 2022"
    std::string           installVersion;  // "17.14.36301.6"; empty when unknown
};

// The environment variables that DECLARE an installation. A developer command
// prompt sets all three; a plain shell sets none.
struct MsvcEnvSnapshot {
    std::filesystem::path vcToolsInstallDir;  // VCToolsInstallDir
    std::filesystem::path vsInstallDir;       // VSINSTALLDIR, else VCINSTALLDIR's parent
    std::filesystem::path clOnPath;           // the first cl.exe on PATH
};

// What the build needs from a toolset for it to count as installed.
struct ToolsetNeeds {
    bool        cl      = true;    // the cl.exe row; clang does not need it
    std::string libArch = "x64";   // lib/<arch> must exist
};

struct ToolsetChoice {
    std::filesystem::path    vsRoot;
    std::filesystem::path    toolsDir;   // <vsRoot>/VC/Tools/MSVC/<version>
    std::string              version;    // "14.44.35207"
    std::string              product;    // for the one line a build prints
    std::string              via;        // the input that decided it
    std::vector<std::string> notes;      // candidates skipped, variables ignored
};

// Dotted versions compared component by component as numbers, so that
// `14.9` < `14.10`. A missing component counts as zero.
int compare_toolset_versions(std::string_view a, std::string_view b);

// `request` matches `version` when every component of `request` equals the
// component at the same position: `14.4` matches `14.4.1` and not `14.44`.
bool toolset_version_matches(std::string_view request, std::string_view version);

// The toolset directory names under `<vsRoot>/VC/Tools/MSVC`, highest first.
std::vector<std::string> toolsets_under(const std::filesystem::path& vsRoot);

// `<vsRoot>/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt`, trimmed;
// empty when the instance does not have one.
std::string default_toolset_of(const std::filesystem::path& vsRoot);

// Does `toolsDir` hold what a build needs: `include/`, `lib/<arch>/`, and
// for the cl.exe row a `cl.exe` under `bin/Host*/<arch>/`.
bool toolset_complete(const std::filesystem::path& toolsDir, const ToolsetNeeds& needs);

// Choose a toolset among the machine's installations.
//
// `selector` is `system` or a toolset version, full or partial.
//
//   system    the first complete candidate of: the toolset VCToolsInstallDir
//             names; the default toolset of the instance VSINSTALLDIR names;
//             the toolset of the first cl.exe on PATH; the default toolset of
//             the instance with the highest installation version. This is the
//             order clang's driver and this module used to apply separately,
//             merged, so that where the two agreed the answer is unchanged.
//   version   the highest complete toolset whose version matches, across every
//             instance. The environment takes no part, and a variable that
//             would have chosen differently is reported in `notes`.
//
// nullopt when no installed toolset qualifies; the caller decides whether that
// is a refusal or a reason to use an ecosystem package.
std::optional<ToolsetChoice>
select_system_toolset(const std::vector<VsInstance>& instances,
                      const MsvcEnvSnapshot&         env,
                      std::string_view               selector,
                      const ToolsetNeeds&            needs);

// One line per complete toolset on the machine, for a refusal to list.
std::vector<std::string> describe_system_toolsets(const std::vector<VsInstance>& instances,
                                                  const ToolsetNeeds& needs);

// vswhere's `-format text` output, one instance per `instanceId:` line. Text
// rather than JSON because this module must not import the JSON library: on
// clang with the MSVC STL, that import changes which `std::optional<std::string>`
// the importers of this module see and breaks their implicit copies.
std::vector<VsInstance> parse_vswhere_text(std::string_view text);

// The Windows half: every instance vswhere reports (prerelease included), plus
// the one VSINSTALLDIR names, plus the conventional paths when vswhere is
// absent. Empty off Windows.
std::vector<VsInstance> enumerate_vs_instances();
MsvcEnvSnapshot msvc_env_snapshot();

// The machine's installation of a pinned toolset, or nullopt. This is what
// makes `msvc@<version>` take an installed toolset before an ecosystem package.
// `notes`, when given, receives what the selection skipped or ignored.
std::optional<MsvcInstallation>
system_installation_matching(std::string_view version, const ToolsetNeeds& needs,
                             std::vector<std::string>* notes = nullptr);

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

// Which origin produced this cl.exe, answered by where the binary lives.
//
// Not a guess: an xlings store is a specific directory layout that mcpp
// itself created, and `xpkgs_from_compiler` recognises it or does not. A
// compiler outside every store came from the machine.
Origin origin_of(const std::filesystem::path& clPath);

// The Windows SDK for a located cl.exe, chosen BY ORIGIN.
//
// This is the second of the three Windows axes (see
// .agents/docs/2026-08-16-windows-toolchain-three-axes-design.md §2), and
// until now it had no identity at all: `find_windows_sdk()` scanned, and
// whatever the scan reached first won — for both origins.
//
//   MANAGED (`msvc@<toolset>`)  the SDK is a DECLARED dependency of the
//                               toolset, installed into the same store. It is
//                               bound, not searched: `WindowsSdkDir` /
//                               `WindowsSdkVersion` do not participate,
//                               because a pin that the environment can
//                               overwrite is not a pin. Two machines building
//                               the same manifest must see the same headers.
//   SYSTEM  (`msvc@system`)     the machine's own SDK, and a machine's things
//                               can only be found by looking. Unchanged: the
//                               declared `WindowsSdkDir` still outranks the
//                               scan, exactly as it does for VSINSTALLDIR.
//
// `note` is non-empty when something the user could have expected to matter
// did not, and the caller MUST surface it. There are exactly two:
//   - a managed toolset ignoring a `WindowsSdkDir` that was set
//   - a managed toolset with NO SDK payload beside it, falling back to the
//     machine's — which works, and is not reproducible, so it says so
struct SdkChoice {
    std::optional<WindowsSdk> sdk;
    Origin                    origin = Origin::Managed;
    std::string               note;
};
SdkChoice resolve_sdk_for(const std::filesystem::path& clPath);

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

#endif // _WIN32

} // namespace

// The three below answer from the same selection every row uses
// (`select_system_toolset`, selector `system`), so the instance, the toolset
// directory and the `std.ixx` they report are one toolset rather than the
// results of three searches. `cl` is not required: the clang row reads them
// too, and a toolset is complete for it without `cl.exe`.
#if defined(_WIN32)
namespace {
std::optional<ToolsetChoice> default_system_choice() {
    ToolsetNeeds needs;
    needs.cl = false;
    return select_system_toolset(enumerate_vs_instances(), msvc_env_snapshot(),
                                 "system", needs);
}
} // namespace
#endif

std::optional<std::filesystem::path> find_vs_install_path() {
#if defined(_WIN32)
    if (auto c = default_system_choice()) return c->vsRoot;
#endif
    return std::nullopt;
}

std::optional<std::filesystem::path> find_msvc_tools_dir() {
#if defined(_WIN32)
    if (auto c = default_system_choice()) return c->toolsDir;
#endif
    return std::nullopt;
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
                            const std::filesystem::path& tools,
                            bool identifyVersion = true);

// Capture cl.exe's banner. cl prints it (plus a usage complaint) when run
// bare; the exit status is irrelevant — parse whatever came out.
std::string capture_cl_banner(const std::filesystem::path& cl) {
    auto r = mcpp::platform::process::capture(
        "\"" + cl.string() + "\" 2>&1");
    return r.output;
}

std::optional<MsvcInstallation>
installation_from_tools_dir(const std::filesystem::path& vsRoot,
                            const std::filesystem::path& tools,
                            bool identifyVersion) {
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
    if (identifyVersion) {
        if (auto parsed = parse_cl_banner(capture_cl_banner(inst.clPath))) {
            inst.clVersion = parsed->first;
            if (!parsed->second.empty()) inst.arch = parsed->second;
        }
    }
    return inst;
}

} // namespace

std::optional<MsvcInstallation> installation_at(const std::filesystem::path& vsRoot,
                                                std::string_view toolsVersion,
                                                bool identifyVersion) {
    if (toolsVersion.empty()) return std::nullopt;
    auto tools = vsRoot / "VC" / "Tools" / "MSVC" / std::string(toolsVersion);
    std::error_code ec;
    if (!std::filesystem::is_directory(tools, ec)) return std::nullopt;
    return installation_from_tools_dir(vsRoot, tools, identifyVersion);
}

// ─── Toolset selection ───────────────────────────────────────────────────

namespace {

// The helpers below each have one caller and live here rather than in the
// exported purview on purpose: an inline helper in the purview is emitted by
// every importer, and that shape has crashed clang 20.1.7 on Windows before
// (see the note beside `toml.cppm`'s `[c-abi-absent]` parser).

void version_components(std::string_view v, std::vector<std::uint64_t>& out) {
    out.clear();
    std::uint64_t n = 0;
    bool any = false;
    for (char c : v) {
        if (c == '.') { out.push_back(n); n = 0; any = false; continue; }
        if (c < '0' || c > '9') break;
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
        any = true;
    }
    if (any || out.empty() || (!v.empty() && v.back() == '.')) out.push_back(n);
}

// `VC/Tools/MSVC/<version>` → the instance root four levels up.
std::filesystem::path vs_root_of_tools(const std::filesystem::path& toolsDir) {
    return toolsDir.parent_path().parent_path().parent_path().parent_path();
}

// A path from the environment may end in a separator
// (`VCToolsInstallDir=...\14.44.35207\`), which leaves `filename()` empty.
std::filesystem::path without_trailing_separator(std::filesystem::path p) {
    if (!p.empty() && p.filename().empty()) p = p.parent_path();
    return p;
}

std::string product_of(const VsInstance& inst) {
    if (!inst.product.empty()) return inst.product;
    auto derived = product_from_vs_root(inst.root);
    return derived.empty() ? inst.root.string() : "Visual Studio " + derived;
}

// The choice an instance offers on its own: its default toolset when that is
// complete, else its highest complete one. Empty version when it has none.
void instance_choice(const VsInstance& inst, const ToolsetNeeds& needs,
                     ToolsetChoice& out) {
    out = ToolsetChoice{};
    const auto base = inst.root / "VC" / "Tools" / "MSVC";
    const auto def  = default_toolset_of(inst.root);
    if (!def.empty() && toolset_complete(base / def, needs)) {
        out.version = def;
    } else {
        for (auto const& v : toolsets_under(inst.root)) {
            if (toolset_complete(base / v, needs)) { out.version = v; break; }
        }
    }
    if (out.version.empty()) return;
    out.vsRoot   = inst.root;
    out.toolsDir = base / out.version;
    out.product  = product_of(inst);
}

bool same_root(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec)) return true;
    return without_trailing_separator(a).lexically_normal()
        == without_trailing_separator(b).lexically_normal();
}

} // namespace

int compare_toolset_versions(std::string_view a, std::string_view b) {
    std::vector<std::uint64_t> x, y;
    version_components(a, x);
    version_components(b, y);
    const auto n = std::max(x.size(), y.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t l = i < x.size() ? x[i] : 0;
        const std::uint64_t r = i < y.size() ? y[i] : 0;
        if (l != r) return l < r ? -1 : 1;
    }
    return 0;
}

bool toolset_version_matches(std::string_view request, std::string_view version) {
    if (request.empty()) return false;
    std::size_t i = 0, j = 0;
    while (true) {
        auto re = request.find('.', i);
        auto ve = version.find('.', j);
        auto rc = request.substr(i, re == std::string_view::npos ? std::string_view::npos : re - i);
        auto vc = j <= version.size()
            ? version.substr(j, ve == std::string_view::npos ? std::string_view::npos : ve - j)
            : std::string_view{};
        if (rc != vc) return false;
        if (re == std::string_view::npos) return true;
        if (ve == std::string_view::npos) return false;
        i = re + 1;
        j = ve + 1;
    }
}

std::vector<std::string> toolsets_under(const std::filesystem::path& vsRoot) {
    std::vector<std::string> out;
    std::error_code ec;
    const auto base = vsRoot / "VC" / "Tools" / "MSVC";
    for (auto& e : std::filesystem::directory_iterator(base, ec)) {
        if (e.is_directory(ec)) out.push_back(e.path().filename().string());
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return compare_toolset_versions(a, b) > 0;
    });
    return out;
}

std::string default_toolset_of(const std::filesystem::path& vsRoot) {
    std::ifstream in(vsRoot / "VC" / "Auxiliary" / "Build"
                     / "Microsoft.VCToolsVersion.default.txt");
    std::string line;
    if (!in || !std::getline(in, line)) return {};
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'
                             || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    return line;
}

bool toolset_complete(const std::filesystem::path& toolsDir, const ToolsetNeeds& needs) {
    std::error_code ec;
    if (!std::filesystem::is_directory(toolsDir / "include", ec)) return false;
    if (!std::filesystem::is_directory(toolsDir / "lib" / needs.libArch, ec)) return false;
    if (!needs.cl) return true;
    for (auto host : {"Hostx64", "Hostarm64", "Hostx86"}) {
        if (std::filesystem::exists(toolsDir / "bin" / host / needs.libArch / "cl.exe", ec))
            return true;
    }
    return false;
}

std::optional<ToolsetChoice>
select_system_toolset(const std::vector<VsInstance>& instances,
                      const MsvcEnvSnapshot&         env,
                      std::string_view               selector,
                      const ToolsetNeeds&            needs) {
    std::vector<std::string> notes;

    // The instances in the order "newest installation first"; an instance
    // whose installer did not report a version sorts last.
    std::vector<const VsInstance*> ranked;
    for (auto const& i : instances) ranked.push_back(&i);
    std::stable_sort(ranked.begin(), ranked.end(),
        [](const VsInstance* a, const VsInstance* b) {
            if (a->installVersion.empty() != b->installVersion.empty())
                return b->installVersion.empty();
            return compare_toolset_versions(a->installVersion, b->installVersion) > 0;
        });

    auto product_for_root = [&](const std::filesystem::path& root) {
        for (auto const* i : ranked)
            if (same_root(i->root, root)) return product_of(*i);
        VsInstance bare;
        bare.root = root;
        return product_of(bare);
    };

    const auto vcTools = without_trailing_separator(env.vcToolsInstallDir);

    if (selector != "system") {
        // PINNED. Every instance is a candidate and the environment is not.
        ToolsetChoice best;
        for (auto const* inst : ranked) {
            for (auto const& v : toolsets_under(inst->root)) {
                if (!toolset_version_matches(selector, v)) continue;
                const auto dir = inst->root / "VC" / "Tools" / "MSVC" / v;
                if (!toolset_complete(dir, needs)) {
                    notes.push_back(std::format(
                        "{} under {} is incomplete for this build and was skipped",
                        v, product_of(*inst)));
                    continue;
                }
                // Strictly greater: on a tie the newer instance, seen first, stays.
                if (best.version.empty() || compare_toolset_versions(v, best.version) > 0) {
                    best.vsRoot   = inst->root;
                    best.toolsDir = dir;
                    best.version  = v;
                    best.product  = product_of(*inst);
                }
            }
        }
        if (best.version.empty()) return std::nullopt;
        best.via = "pinned";
        if (!vcTools.empty() && vcTools.filename().string() != best.version) {
            notes.push_back(std::format(
                "VCToolsInstallDir ({}) is ignored: the toolset is pinned to {}",
                vcTools.filename().string(), selector));
        }
        best.notes = std::move(notes);
        return best;
    }

    // SYSTEM. The declarations first, then the installer's newest instance.
    if (!vcTools.empty()) {
        if (toolset_complete(vcTools, needs)) {
            ToolsetChoice c;
            c.toolsDir = vcTools;
            c.vsRoot   = vs_root_of_tools(vcTools);
            c.version  = vcTools.filename().string();
            c.product  = product_for_root(c.vsRoot);
            c.via      = "VCToolsInstallDir";
            c.notes    = std::move(notes);
            return c;
        }
        notes.push_back(std::format(
            "VCToolsInstallDir ({}) names an incomplete toolset and was skipped",
            vcTools.string()));
    }
    if (!env.vsInstallDir.empty()) {
        VsInstance declared;
        declared.root    = without_trailing_separator(env.vsInstallDir);
        declared.product = product_for_root(declared.root);
        ToolsetChoice c;
        instance_choice(declared, needs, c);
        if (!c.version.empty()) {
            c.via   = "VSINSTALLDIR";
            c.notes = std::move(notes);
            return c;
        }
        notes.push_back(std::format(
            "VSINSTALLDIR ({}) has no complete toolset and was skipped",
            declared.root.string()));
    }
    if (!env.clOnPath.empty()) {
        // <tools>/bin/Host<h>/<t>/cl.exe
        const auto tools = env.clOnPath.parent_path().parent_path()
                               .parent_path().parent_path();
        if (toolset_complete(tools, needs)) {
            ToolsetChoice c;
            c.toolsDir = tools;
            c.vsRoot   = vs_root_of_tools(tools);
            c.version  = tools.filename().string();
            c.product  = product_for_root(c.vsRoot);
            c.via      = "PATH";
            c.notes    = std::move(notes);
            return c;
        }
    }
    for (auto const* inst : ranked) {
        ToolsetChoice c;
        instance_choice(*inst, needs, c);
        if (c.version.empty()) continue;
        c.via   = "newest instance";
        c.notes = std::move(notes);
        return c;
    }
    return std::nullopt;
}

std::vector<std::string> describe_system_toolsets(const std::vector<VsInstance>& instances,
                                                  const ToolsetNeeds& needs) {
    std::vector<std::string> out;
    for (auto const& inst : instances) {
        const auto def = default_toolset_of(inst.root);
        for (auto const& v : toolsets_under(inst.root)) {
            if (!toolset_complete(inst.root / "VC" / "Tools" / "MSVC" / v, needs)) continue;
            out.push_back(std::format("{:<14}{}{}", v, product_of(inst),
                                      v == def ? " (default)" : ""));
        }
    }
    return out;
}

std::vector<VsInstance> parse_vswhere_text(std::string_view text) {
    std::vector<VsInstance> out;
    bool open = false;
    while (!text.empty()) {
        auto nl   = text.find('\n');
        auto line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        auto colon = line.find(": ");
        if (colon == std::string_view::npos) continue;
        auto key   = line.substr(0, colon);
        auto value = std::string(line.substr(colon + 2));
        if (key == "instanceId") {
            out.emplace_back();
            open = true;
        } else if (!open) {
            continue;
        } else if (key == "installationPath") {
            out.back().root = std::filesystem::path(
                std::u8string(reinterpret_cast<const char8_t*>(value.c_str())));
        } else if (key == "installationVersion") {
            out.back().installVersion = std::move(value);
        } else if (key == "displayName") {
            out.back().product = std::move(value);
        }
    }
    std::erase_if(out, [](const VsInstance& i) { return i.root.empty(); });
    return out;
}

std::vector<VsInstance> enumerate_vs_instances() {
    std::vector<VsInstance> out;
#if defined(_WIN32)
    auto add = [&](VsInstance inst) {
        for (auto const& e : out)
            if (same_root(e.root, inst.root)) return;
        out.push_back(std::move(inst));
    };
    const std::filesystem::path vswhere =
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    bool listed = false;
    if (std::filesystem::exists(vswhere)) {
        // `-all -prerelease`: every instance, Insiders included, complete or
        // not; completeness is judged per toolset below. `-utf8` because an
        // installation path may carry characters the console code page cannot.
        auto r = mcpp::platform::process::capture(
            "\"" + vswhere.string() + "\" -all -prerelease -products * "
            "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
            "-format text -utf8 2>nul");
        // vswhere ran: an empty list is an answer (no instance has the C++
        // tools), so the conventional paths are not consulted behind it.
        if (r.exit_code == 0) {
            listed = true;
            for (auto& inst : parse_vswhere_text(r.output)) add(std::move(inst));
        }
    }
    if (auto p = find_vs_via_vsinstalldir()) add(VsInstance{*p, {}, {}});
    if (!listed) {
        if (auto p = find_vs_via_comntools()) add(VsInstance{*p, {}, {}});
        if (auto p = find_vs_via_paths())     add(VsInstance{*p, {}, {}});
    }
#endif
    return out;
}

MsvcEnvSnapshot msvc_env_snapshot() {
    MsvcEnvSnapshot env;
#if defined(_WIN32)
    if (auto* v = std::getenv("VCToolsInstallDir"); v && *v) env.vcToolsInstallDir = v;
    if (auto* v = std::getenv("VSINSTALLDIR"); v && *v) {
        env.vsInstallDir = v;
    } else if (auto* v2 = std::getenv("VCINSTALLDIR"); v2 && *v2) {
        env.vsInstallDir = without_trailing_separator(v2).parent_path();
    }
    if (auto* path = std::getenv("PATH"); path && *path) {
        std::string_view rest = path;
        while (!rest.empty()) {
            auto semi = rest.find(';');
            auto dir  = rest.substr(0, semi);
            if (!dir.empty()) {
                std::filesystem::path cl = std::filesystem::path(std::string(dir)) / "cl.exe";
                std::error_code ec;
                if (std::filesystem::exists(cl, ec)) { env.clOnPath = cl; break; }
            }
            if (semi == std::string_view::npos) break;
            rest.remove_prefix(semi + 1);
        }
    }
#endif
    return env;
}

std::optional<MsvcInstallation>
system_installation_matching(std::string_view version, const ToolsetNeeds& needs,
                             std::vector<std::string>* notes) {
    auto choice = select_system_toolset(enumerate_vs_instances(), msvc_env_snapshot(),
                                        version, needs);
    if (!choice) return std::nullopt;
    if (notes) *notes = choice->notes;
    return installation_from_tools_dir(choice->vsRoot, choice->toolsDir);
}

std::optional<MsvcInstallation> detect_installation() {
#if defined(_WIN32)
    auto choice = select_system_toolset(enumerate_vs_instances(), msvc_env_snapshot(),
                                        "system", ToolsetNeeds{});
    if (!choice) return std::nullopt;
    return installation_from_tools_dir(choice->vsRoot, choice->toolsDir);
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

namespace {

// Highest version dir under `root/Include` that actually carries the UCRT
// headers; `want` (from WindowsSdkVersion) wins if it is one of them.
std::optional<WindowsSdk> pick_sdk_in(const std::filesystem::path& root,
                                      std::string_view want) {
    std::error_code ec;
    auto inc = root / "Include";
    if (!std::filesystem::is_directory(inc, ec)) return std::nullopt;
    auto usable = [&](const std::filesystem::path& verDir, const std::string& v) {
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
}

// `WindowsSdkVersion` without its trailing backslash (vcvars exports one; it
// is not part of the directory name). Empty when unset.
std::string declared_sdk_version() {
    std::string want;
    if (auto* v = std::getenv("WindowsSdkVersion"); v && *v) {
        want = v;
        while (!want.empty() && (want.back() == '\\' || want.back() == '/'))
            want.pop_back();
    }
    return want;
}

} // namespace

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
    //
    // The version-selection rule itself lives in `pick_sdk_in`, because the
    // MANAGED origin needs the same rule applied to a different (and much
    // shorter) list of roots — see `resolve_sdk_for`.

    // 1. Declared: WindowsSdkDir (+ WindowsSdkVersion). vcvars exports both;
    //    WindowsSdkVersion carries a trailing backslash there, which is not
    //    part of the directory name.
    const std::string want = declared_sdk_version();
    if (auto* dir = std::getenv("WindowsSdkDir"); dir && *dir) {
        if (auto s = pick_sdk_in(std::filesystem::path{dir}, want)) return s;
    }

    // 2. Roots the caller knows about (managed toolset's own store).
    for (const auto& root : extraRoots)
        if (auto s = pick_sdk_in(root, want)) return s;

    // 3. The conventional absolute install roots.
    for (const char* base : {"C:\\Program Files (x86)\\Windows Kits\\10",
                             "C:\\Program Files\\Windows Kits\\10"}) {
        if (auto s = pick_sdk_in(std::filesystem::path{base}, want)) return s;
    }
    return std::nullopt;
}

Origin origin_of(const std::filesystem::path& clPath) {
    return mcpp::xlings::paths::xpkgs_from_compiler(clPath)
        ? Origin::Managed : Origin::SystemMsvc;
}

SdkChoice resolve_sdk_for(const std::filesystem::path& clPath) {
    SdkChoice out;
    out.origin = origin_of(clPath);
    auto siblings = sibling_sdk_roots(clPath);

    if (out.origin == Origin::SystemMsvc) {
        // A machine's things can only be found by looking, and a declared
        // WindowsSdkDir is still the most specific answer available.
        // `siblings` is empty here by construction (a system cl.exe is in no
        // store); passed through so the two origins share one call shape.
        out.sdk = find_windows_sdk(siblings);
        return out;
    }

    // MANAGED. The SDK arrived as a declared dependency of this toolset and
    // sits in the same store — so it is looked up, not searched for, and
    // nothing in the environment gets a vote. See the design doc's §2.1 for
    // what searching cost: a half-unpacked payload outranked the machine's own
    // complete SDK because its version number was higher, every TU compiled,
    // and the build died at LNK1104 with nothing in the log naming the SDK.
    //
    // `sibling_sdk_roots` returns newest first, so the first hit is also the
    // one the highest-version rule would have chosen.
    for (auto const& root : siblings) {
        // `want` deliberately empty: WindowsSdkVersion is the same declaration
        // channel as WindowsSdkDir, and letting it pick among payloads is the
        // same override wearing a smaller hat.
        if ((out.sdk = pick_sdk_in(root, {}))) break;
    }

    if (out.sdk) {
        const char* dir = std::getenv("WindowsSdkDir");
        const char* ver = std::getenv("WindowsSdkVersion");
        if ((dir && *dir) || (ver && *ver)) {
            out.note = std::format(
                "WindowsSdkDir/WindowsSdkVersion in the environment "
                "({}) is ignored: this build pins a managed MSVC toolset, and "
                "the Windows SDK it was installed with ({} at {}) is part of "
                "that pin. Use msvc@system if the machine's SDK is what you "
                "want.",
                dir && *dir ? dir : ver,
                out.sdk->version, out.sdk->root.string());
        }
        return out;
    }

    // No SDK payload beside the toolset. Falling back keeps the build working
    // — but quietly falling back is precisely how the version axis stopped
    // meaning anything one layer down, so it is reported.
    out.sdk = find_windows_sdk({});
    out.note = out.sdk
        ? std::format(
              "no windows-sdk payload was found beside this managed MSVC "
              "toolset, so the machine's SDK ({} at {}) is being used. That "
              "makes the build depend on this machine — reinstall the toolset "
              "(`mcpp toolchain install msvc <version>`) to pull its own SDK.",
              out.sdk->version, out.sdk->root.string())
        : std::string{};
    return out;
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
        // identifyVersion=false: this asks IF a toolset is here, never which.
        if (installation_at(v.path(), v.path().filename().string(),
                            /*identifyVersion=*/false)) return true;
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

int std_module_min_level_for_stl(const std::filesystem::path& stdModuleSource) {
    // <VS>/VC/Tools/MSVC/<toolset>/modules/std.ixx -> up two from the file.
    if (stdModuleSource.empty()) return 23;
    auto toolset = stdModuleSource.parent_path().parent_path().filename().string();
    int major = 0, minor = 0;
    std::size_t i = 0;
    auto read = [&](int& out) {
        bool any = false;
        while (i < toolset.size() && toolset[i] >= '0' && toolset[i] <= '9') {
            out = out * 10 + (toolset[i] - '0');
            ++i;
            any = true;
        }
        return any;
    };
    if (!read(major)) return 23;
    if (i < toolset.size() && toolset[i] == '.') ++i;
    if (!read(minor)) return 23;
    // 14 is the toolset major for every MSVC since VS 2015 and the only value
    // this mapping is defined for. Anything else is a layout this code does
    // not recognise, and a guess there would be the defect it replaces.
    if (major != 14) return 23;
    return minor >= 38 ? 20 : 23;
}

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
    // THE STD MODULE OF THIS TOOLSET, OR NONE. A toolset without
    // `modules/std.ixx` used to borrow the one the machine-wide search found,
    // which on a machine with two installations compiled one toolset's
    // `std.ixx` against another toolset's headers. Absent means no `import std`
    // for this toolset, and the caller says so.
    std::error_code ec;
    if (auto ixx = toolsDir / "modules" / "std.ixx";
        std::filesystem::exists(ixx, ec)) {
        tc.stdModuleSource = ixx;
        tc.hasImportStd    = true;
    }
    if (tc.hasImportStd) {
        // The STL, not the banner. For a real cl installation the two agree by
        // construction -- `std.ixx` was found under the same toolset directory
        // cl came from -- and a unit test asserts that. They separate only when
        // `find_msvc_tools_dir()` and the selected module source disagree, and
        // there the file that will be compiled is the correct answer.
        tc.importStdMinLevel = std_module_min_level_for_stl(tc.stdModuleSource);
    }
    if (auto compat = toolsDir / "modules" / "std.compat.ixx";
        std::filesystem::exists(compat, ec)) {
        tc.stdCompatSource = compat;
    }

    // Build environment (INCLUDE/LIB/PATH/VSLANG). SDK absence keeps
    // detection working (selection UX on SDK-less boxes); the build path
    // errors with guidance when envOverrides is empty.
    //
    // BY ORIGIN, not by search. A managed toolset carries its own SDK as an
    // xlings dependency and the compiler's own path says which store it is in,
    // so the two halves of a pinned toolchain stay together — and stay
    // together even when the environment says otherwise, which is the part a
    // search could not give. `msvc@system` keeps the search chain: the
    // machine's things can only be found by looking.
    auto choice = resolve_sdk_for(tc.binaryPath);
    if (choice.sdk) {
        tc.envOverrides = build_env_for_cl(tc.binaryPath, parsed->second,
                                           *choice.sdk);
        tc.windowsSdkVersion = choice.sdk->version;
    }
    if (!choice.note.empty()) tc.resolutionNote = choice.note;

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
