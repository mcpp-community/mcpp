// mcpp.toolchain.registry - user spec and package/provider mapping.

export module mcpp.toolchain.registry;

import std;
import mcpp.toolchain.clang;
import mcpp.toolchain.gcc;
import mcpp.toolchain.llvm;
import mcpp.toolchain.model;

export namespace mcpp::toolchain {

struct ToolchainSpec {
    std::string compiler;    // user-facing compiler name, namespace-stripped
    std::string version;     // user-facing version, may include -musl
    bool        isMusl = false;
};

struct XimToolchainPackage {
    std::string                     ximName;
    std::string                     ximVersion;
    std::string                     displayCompiler;
    std::string                     displayVersion;
    std::vector<std::string>        frontendCandidates;
    bool                            needsGccPostInstallFixup = false;

    std::string target() const {
        return std::format("xim:{}@{}", ximName, ximVersion);
    }

    std::string display_spec() const {
        return std::format("{}@{}", displayCompiler, displayVersion);
    }
};

std::expected<ToolchainSpec, std::string>
parse_toolchain_spec(std::string compilerArg,
                     std::string versionArg = {},
                     bool requireCompiler = true);

XimToolchainPackage to_xim_package(const ToolchainSpec& spec);

ToolchainSpec with_resolved_xim_version(const ToolchainSpec& spec,
                                        std::string_view ximVersion);

std::filesystem::path toolchain_frontend(const std::filesystem::path& binDir,
                                         std::string_view compiler);

std::filesystem::path toolchain_frontend(const std::filesystem::path& binDir,
                                         const XimToolchainPackage& pkg);

std::string display_label(std::string_view compiler, std::string_view version);
bool matches_default_toolchain(std::string_view configuredDefault,
                               std::string_view compiler,
                               std::string_view version);

std::vector<std::pair<std::string, std::string>> available_toolchain_indexes();

std::filesystem::path derive_c_compiler(const Toolchain& tc);
std::filesystem::path archive_tool(const Toolchain& tc);
std::filesystem::path staged_std_bmi_path(const Toolchain& tc,
                                          const std::filesystem::path& outputDir);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

namespace {

bool ends_with(std::string_view s, std::string_view suf) {
    return s.size() >= suf.size()
        && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::string strip_namespace(std::string compiler) {
    if (auto colon = compiler.find(':'); colon != std::string::npos)
        return compiler.substr(colon + 1);
    return compiler;
}

std::vector<std::string> frontend_candidates_for(std::string_view ximName,
                                                 bool isMusl) {
    if (isMusl) return {"x86_64-linux-musl-g++", "g++"};
    if (ximName == "gcc") return {"g++"};
    if (ximName == "llvm") return mcpp::toolchain::llvm::frontend_candidates();
    if (ximName == "msvc") return {"cl.exe"};
    return {"g++", "clang++", "x86_64-linux-musl-g++", "cl.exe"};
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

} // namespace

std::expected<ToolchainSpec, std::string>
parse_toolchain_spec(std::string compilerArg,
                     std::string versionArg,
                     bool requireCompiler) {
    if (auto at = compilerArg.find('@'); at != std::string::npos) {
        if (versionArg.empty()) versionArg = compilerArg.substr(at + 1);
        compilerArg = compilerArg.substr(0, at);
    }

    compilerArg = strip_namespace(std::move(compilerArg));
    if (compilerArg.empty() && requireCompiler) {
        return std::unexpected("missing compiler name");
    }

    ToolchainSpec spec;
    spec.compiler = std::move(compilerArg);
    spec.version  = std::move(versionArg);
    spec.isMusl   = spec.compiler == "musl-gcc" || ends_with(spec.version, "-musl");
    return spec;
}

XimToolchainPackage to_xim_package(const ToolchainSpec& spec) {
    XimToolchainPackage pkg;
    pkg.displayCompiler = spec.compiler;
    pkg.displayVersion  = spec.version;

    std::string ximCompiler = spec.compiler;
    if (mcpp::toolchain::llvm::is_alias(ximCompiler))
        ximCompiler = mcpp::toolchain::llvm::package_name();

    pkg.ximName = ximCompiler;
    pkg.ximVersion = spec.version;

    if (spec.isMusl) {
        if (pkg.ximName != "musl-gcc")
            pkg.ximName = "musl-" + pkg.ximName;
        if (ends_with(pkg.ximVersion, "-musl"))
            pkg.ximVersion.resize(pkg.ximVersion.size() - 5);
    }

    pkg.frontendCandidates = frontend_candidates_for(pkg.ximName, spec.isMusl);
    pkg.needsGccPostInstallFixup = spec.compiler == "gcc" && !spec.isMusl;
    return pkg;
}

ToolchainSpec with_resolved_xim_version(const ToolchainSpec& spec,
                                        std::string_view ximVersion) {
    ToolchainSpec out = spec;
    out.version = std::string(ximVersion);
    if (out.isMusl) out.version += "-musl";
    return out;
}

std::filesystem::path toolchain_frontend(const std::filesystem::path& binDir,
                                         std::string_view compiler) {
    bool isMusl = compiler == "musl-gcc";
    std::string ximName(compiler);
    if (mcpp::toolchain::llvm::is_alias(ximName))
        ximName = mcpp::toolchain::llvm::package_name();

    for (auto& cand : frontend_candidates_for(ximName, isMusl)) {
        auto p = binDir / cand;
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

std::filesystem::path toolchain_frontend(const std::filesystem::path& binDir,
                                         const XimToolchainPackage& pkg) {
    for (auto& cand : pkg.frontendCandidates) {
        auto p = binDir / cand;
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

std::string display_label(std::string_view compiler, std::string_view version) {
    if (compiler == "musl-gcc")
        return std::format("gcc {}-musl", version);
    return std::format("{} {}", compiler, version);
}

bool matches_default_toolchain(std::string_view configuredDefault,
                               std::string_view compiler,
                               std::string_view version) {
    if (configuredDefault == std::format("{}@{}", compiler, version)) return true;
    if (compiler == "musl-gcc"
        && configuredDefault == std::format("gcc@{}-musl", version)) {
        return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> available_toolchain_indexes() {
    return {
        {"gcc",      "gcc"},
        {"musl-gcc", "musl-gcc"},
        {mcpp::toolchain::llvm::package_name(), "llvm"},
    };
}

std::filesystem::path derive_c_compiler(const Toolchain& tc) {
    return derive_c_compiler_path(tc.binaryPath);
}

std::filesystem::path archive_tool(const Toolchain& tc) {
    if (is_clang(tc)) return mcpp::toolchain::clang::archive_tool(tc);

    if (!is_musl_target(tc)) {
        if (auto binutilsBin = mcpp::toolchain::gcc::find_binutils_bin(tc.binaryPath))
            return *binutilsBin / "ar";
    }

    auto muslAr = tc.binaryPath.parent_path() / "x86_64-linux-musl-ar";
    if (std::filesystem::exists(muslAr)) return muslAr;
    return {};
}

std::filesystem::path staged_std_bmi_path(const Toolchain& tc,
                                          const std::filesystem::path& outputDir) {
    if (is_clang(tc)) return mcpp::toolchain::clang::staged_std_bmi_path(outputDir);
    return mcpp::toolchain::gcc::staged_std_bmi_path(outputDir);
}

} // namespace mcpp::toolchain
