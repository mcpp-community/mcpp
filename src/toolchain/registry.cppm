// mcpp.toolchain.registry — the two-axis toolchain identity model and its
// payload mapping.
//
// Identity (design §4.1–§4.3): a toolchain is `family@version` (family ∈
// gcc | llvm | msvc), a target is a canonical Triple (triple.cppm). The two
// axes are orthogonal: "cross", "musl" and "mingw" are NOT names — the
// variant lives in the target's env segment, and cross is the host≠target
// relation. Which xim PACKAGE serves a (family, version, target, host)
// combination is a data mapping below — that's the distribution layer, where
// names like `mingw-cross-gcc` are current identity (not legacy) and stay.
//
// Legacy spellings (musl-gcc, gcc@V-musl, mingw, mingw-cross, clang,
// <triple>-gcc) are normalized by mcpp.toolchain.compat before this module
// ever sees them; core code deals in canonical form only.

export module mcpp.toolchain.registry;

import std;
import mcpp.platform;
import mcpp.toolchain.clang;
import mcpp.toolchain.compat;
import mcpp.toolchain.gcc;
import mcpp.toolchain.llvm;
import mcpp.toolchain.model;
import mcpp.toolchain.msvc;
import mcpp.toolchain.triple;

export namespace mcpp::toolchain {

enum class Family { Gcc, Llvm, Msvc };

inline std::string_view family_name(Family f) {
    switch (f) {
        case Family::Gcc:  return "gcc";
        case Family::Llvm: return "llvm";
        case Family::Msvc: return "msvc";
    }
    return "?";
}

struct ToolchainSpec {
    Family          family = Family::Gcc;
    std::string     version;      // numeric (possibly partial), or "system"
    triple::Triple  target;       // empty = host
    // One-line canonical hint when the input used a legacy spelling
    // (compat.cppm); empty otherwise. Printed at most once per process by
    // print_compat_hint().
    std::string     compatHint;

    bool is_host_target() const { return target.empty(); }

    // "gcc@16.1.0" — the toolchain axis alone (config persistence, matching).
    std::string spec_str() const {
        return std::format("{}@{}", family_name(family), version);
    }

    // "gcc@16.1.0" or "gcc@16.1.0 → x86_64-windows-gnu" — user-facing.
    std::string display() const {
        if (target.empty()) return spec_str();
        return std::format("{} → {}", spec_str(), target.str());
    }
};

struct XimToolchainPackage {
    std::string                     ximName;
    std::string                     ximVersion;
    std::string                     displaySpec;   // canonical, from the spec
    std::vector<std::string>        frontendCandidates;
    bool                            needsGccPostInstallFixup = false;

    std::string target() const {
        return std::format("xim:{}@{}", ximName, ximVersion);
    }

    std::string display_spec() const { return displaySpec; }
};

std::expected<ToolchainSpec, std::string>
parse_toolchain_spec(std::string compilerArg,
                     std::string versionArg = {},
                     bool requireCompiler = true);

// Print the spec's compat hint (once per process; no-op for canonical input).
void print_compat_hint(const ToolchainSpec& spec);

// The (family, target, host) → xim package mapping — the distribution layer.
XimToolchainPackage to_xim_package(const ToolchainSpec& spec);

ToolchainSpec with_resolved_xim_version(const ToolchainSpec& spec,
                                        std::string_view ximVersion);

std::filesystem::path toolchain_frontend(const std::filesystem::path& binDir,
                                         const XimToolchainPackage& pkg);

// Reverse mapping: an installed `xim-x-<name>` payload directory back to its
// (family, target) identity. nullopt for non-toolchain xpkgs (ninja, glibc,
// python, …) — list/doctor use this to filter what they enumerate.
struct PayloadIdentity {
    Family          family;
    triple::Triple  target;       // empty = host-target payload (gcc, llvm)
};
std::optional<PayloadIdentity> identify_xim_payload(std::string_view ximDirName);

// Does an installed payload row match the configured default (toolchain axis;
// version exact)? msvc matches on family alone — the persisted spec is the
// stable "msvc@system", never a concrete version.
bool spec_matches_payload(const ToolchainSpec& def,
                          const PayloadIdentity& id,
                          std::string_view payloadVersion);

// System toolchains are located on the machine, never installed/removed by
// mcpp. Today that's MSVC (`msvc@system`); the PATH-compiler escape hatch
// (`[toolchain] … = "system"`) is a separate, older mechanism.
bool is_system_toolchain(const ToolchainSpec& spec);

// xim index names to query for the Available section, with the family each
// one contributes versions to. Host-conditional: a host only lists payloads
// it can install.
struct AvailableIndex {
    std::string ximName;
    Family      family;
};
std::vector<AvailableIndex> available_toolchain_indexes();

std::filesystem::path derive_c_compiler(const Toolchain& tc);
std::filesystem::path archive_tool(const Toolchain& tc);
std::filesystem::path link_tool(const Toolchain& tc);
std::filesystem::path staged_std_bmi_path(const Toolchain& tc,
                                          const std::filesystem::path& outputDir);
std::filesystem::path staged_std_compat_bmi_path(const Toolchain& tc,
                                                 const std::filesystem::path& outputDir);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

namespace {

bool ends_with(std::string_view s, std::string_view suf) {
    return s.size() >= suf.size()
        && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::filesystem::path derive_c_compiler_path(const std::filesystem::path& cxxPath) {
    auto stem = cxxPath.stem().string();
    auto parent = cxxPath.parent_path();
    auto ext = cxxPath.extension();

    std::string cc_stem;
    if (stem.ends_with("++")) {
        cc_stem = stem.substr(0, stem.size() - 2);
        if (cc_stem == "g" || cc_stem.ends_with("-g"))
            cc_stem += "cc";
    } else {
        cc_stem = stem;
    }
    return parent / (cc_stem + ext.string());
}

triple::Triple host_musl_triple() {
    return { std::string(mcpp::platform::host_arch), "linux", "musl" };
}

} // namespace

std::expected<ToolchainSpec, std::string>
parse_toolchain_spec(std::string compilerArg,
                     std::string versionArg,
                     bool requireCompiler) {
    if (auto at = compilerArg.find('@'); at != std::string::npos) {
        if (versionArg.empty()) versionArg = compilerArg.substr(at + 1);
        compilerArg = compilerArg.substr(0, at);
    }
    if (compilerArg.empty() && requireCompiler) {
        return std::unexpected("missing compiler name");
    }

    auto norm = compat::normalize_spec(compilerArg, versionArg);
    if (!norm) {
        return std::unexpected(std::format(
            "unknown toolchain '{}' (expected gcc | llvm | msvc, or a "
            "supported alias like mingw / musl-gcc)", compilerArg));
    }

    ToolchainSpec spec;
    if      (norm->family == "llvm") spec.family = Family::Llvm;
    else if (norm->family == "msvc") spec.family = Family::Msvc;
    else                             spec.family = Family::Gcc;
    spec.version = std::move(norm->version);
    spec.target  = std::move(norm->target);
    if (norm->changed) spec.compatHint = std::move(norm->hint);
    return spec;
}

void print_compat_hint(const ToolchainSpec& spec) {
    if (spec.compatHint.empty()) return;
    compat::print_hint_once(spec.compatHint);
}

XimToolchainPackage to_xim_package(const ToolchainSpec& spec) {
    XimToolchainPackage pkg;
    pkg.displaySpec = spec.display();
    pkg.ximVersion  = spec.version;

    if (spec.family == Family::Msvc) {
        pkg.ximName = "msvc";                 // never resolved via xim
        pkg.frontendCandidates = {"cl.exe"};
        return pkg;
    }
    if (spec.family == Family::Llvm) {
        pkg.ximName = mcpp::toolchain::llvm::package_name();
        pkg.frontendCandidates = mcpp::toolchain::llvm::frontend_candidates();
        return pkg;
    }

    // Family::Gcc — the target decides the payload.
    const auto& t = spec.target;

    if (t.is_musl()) {
        // Same target, two payload shapes: the host-native `musl-gcc` package
        // (XLINGS_RES picks the host-matching asset) when target arch == host
        // arch, else the triple-named cross package. Canonical linux-musl
        // triples coincide with the GNU tool spelling, so `<triple>-g++` is
        // the frontend either way.
        bool native = mcpp::platform::is_linux
                   && t.arch == mcpp::platform::host_arch;
        pkg.ximName = native ? "musl-gcc" : t.str() + "-gcc";
        pkg.frontendCandidates = { t.str() + "-g++", "g++" };
        return pkg;
    }

    if (t.is_windows_gnu()
        || (t.empty() && mcpp::platform::is_windows)) {
        // GCC targeting Windows PE (GNU CRT) — ONE user-facing identity,
        // host-split at the distribution layer only:
        //   Windows host → native winlibs UCRT build (PE frontend g++.exe)
        //   other hosts  → Linux-hosted MSVCRT cross (ELF frontend, triple-
        //                  prefixed so a cross build never silently falls
        //                  back to a native g++)
        if constexpr (mcpp::platform::is_windows) {
            pkg.ximName = "mingw-gcc";
            pkg.frontendCandidates = {"g++.exe", "g++"};
        } else {
            pkg.ximName = "mingw-cross-gcc";
            pkg.frontendCandidates = {"x86_64-w64-mingw32-g++"};
        }
        return pkg;
    }

    // Host target (or linux-gnu): the glibc gcc package.
    pkg.ximName = "gcc";
    pkg.frontendCandidates = {"g++"};
    pkg.needsGccPostInstallFixup = true;
    return pkg;
}

ToolchainSpec with_resolved_xim_version(const ToolchainSpec& spec,
                                        std::string_view ximVersion) {
    ToolchainSpec out = spec;
    out.version = std::string(ximVersion);
    return out;
}

std::filesystem::path toolchain_frontend(const std::filesystem::path& binDir,
                                         const XimToolchainPackage& pkg) {
    for (auto& cand : pkg.frontendCandidates) {
        auto p = binDir / cand;
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

std::optional<PayloadIdentity> identify_xim_payload(std::string_view ximDirName) {
    if (ximDirName == "gcc")
        return PayloadIdentity{ Family::Gcc, {} };
    if (ximDirName == mcpp::toolchain::llvm::package_name())
        return PayloadIdentity{ Family::Llvm, {} };
    if (ximDirName == "musl-gcc")
        return PayloadIdentity{ Family::Gcc, host_musl_triple() };
    if (ximDirName == "mingw-gcc" || ximDirName == "mingw-cross-gcc")
        return PayloadIdentity{ Family::Gcc, { "x86_64", "windows", "gnu" } };
    if (ends_with(ximDirName, "-gcc")) {
        auto prefix = ximDirName.substr(0, ximDirName.size() - 4);
        if (auto t = triple::parse(prefix))
            return PayloadIdentity{ Family::Gcc, *t };
    }
    return std::nullopt;   // not a toolchain payload (ninja, glibc, …)
}

bool spec_matches_payload(const ToolchainSpec& def,
                          const PayloadIdentity& id,
                          std::string_view payloadVersion) {
    if (def.family != id.family) return false;
    if (def.family == Family::Msvc) return true;   // msvc@system: family match
    return def.version == payloadVersion;
}

bool is_system_toolchain(const ToolchainSpec& spec) {
    return spec.family == Family::Msvc;
}

std::vector<AvailableIndex> available_toolchain_indexes() {
    std::vector<AvailableIndex> out{
        { "gcc",      Family::Gcc },
        { "musl-gcc", Family::Gcc },
        { mcpp::toolchain::llvm::package_name(), Family::Llvm },
    };
    // The Windows-PE gcc payload is host-split at the distribution layer
    // (§4.3); each host lists the package it would actually install.
    if constexpr (mcpp::platform::is_windows)
        out.push_back({ "mingw-gcc", Family::Gcc });
    else if constexpr (mcpp::platform::is_linux)
        out.push_back({ "mingw-cross-gcc", Family::Gcc });
    return out;
}

std::filesystem::path derive_c_compiler(const Toolchain& tc) {
    return derive_c_compiler_path(tc.binaryPath);
}

std::filesystem::path archive_tool(const Toolchain& tc) {
    if (tc.compiler == CompilerId::MSVC) {
        auto lib = tc.binaryPath.parent_path() / "lib.exe";
        std::error_code ec;
        if (std::filesystem::exists(lib, ec)) return lib;
        return {};
    }
    if (is_clang(tc)) return mcpp::toolchain::clang::archive_tool(tc);

    // MinGW bundles its own binutils next to the frontend (self-contained,
    // like musl) — never an external binutils xpkg. Native (Windows-host) ships
    // `ar.exe`; the Linux-hosted cross ships the triple-prefixed ELF tool
    // `x86_64-w64-mingw32-ar`. Try the cross form first, then native.
    if (is_mingw_target(tc)) {
        std::error_code ec;
        auto dir = tc.binaryPath.parent_path();
        if (!tc.targetTriple.empty()) {
            auto crossAr = dir / (tc.targetTriple + "-ar");
            if (std::filesystem::exists(crossAr, ec)) return crossAr;
        }
        auto ar = dir / "ar.exe";
        if (std::filesystem::exists(ar, ec)) return ar;
        return {};
    }

    if (!is_musl_target(tc)) {
        if (auto binutilsBin = mcpp::toolchain::gcc::find_binutils_bin(tc.binaryPath))
            return *binutilsBin / "ar";
    }

    // musl `ar` is the triple-prefixed cross tool (e.g. aarch64-linux-musl-ar),
    // sitting next to the frontend. Derive from the resolved target triple so
    // cross targets pick the matching archiver instead of the x86_64 one.
    std::string arName = !tc.targetTriple.empty()
        ? tc.targetTriple + "-ar"
        : "x86_64-linux-musl-ar";
    auto muslAr = tc.binaryPath.parent_path() / arName;
    if (std::filesystem::exists(muslAr)) return muslAr;
    return {};
}

std::filesystem::path staged_std_bmi_path(const Toolchain& tc,
                                          const std::filesystem::path& outputDir) {
    if (tc.compiler == CompilerId::MSVC)
        return mcpp::toolchain::msvc::staged_std_bmi_path(outputDir);
    if (is_clang(tc)) return mcpp::toolchain::clang::staged_std_bmi_path(outputDir);
    return mcpp::toolchain::gcc::staged_std_bmi_path(outputDir);
}

std::filesystem::path staged_std_compat_bmi_path(const Toolchain& tc,
                                                 const std::filesystem::path& outputDir) {
    if (tc.compiler == CompilerId::MSVC)
        return mcpp::toolchain::msvc::staged_std_compat_bmi_path(outputDir);
    return mcpp::toolchain::clang::staged_std_compat_bmi_path(outputDir);
}

// Separate linker binary for SeparateLinker dialects (link.exe beside cl).
// Empty for driver-link toolchains.
std::filesystem::path link_tool(const Toolchain& tc) {
    if (tc.compiler != CompilerId::MSVC) return {};
    auto link = tc.binaryPath.parent_path() / "link.exe";
    std::error_code ec;
    if (std::filesystem::exists(link, ec)) return link;
    return {};
}

} // namespace mcpp::toolchain
