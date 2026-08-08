// mcpp.fallback.probe_sysroot — sysroot detection strategies.
//
// Three strategies for discovering the sysroot:
//   1. Remap GCC's baked-in build-time sysroot to the local xpkgs layout
//   2. Parse Clang's .cfg file for --sysroot=
//   3. macOS: use xcrun to discover the SDK path

module;
#include <cstdlib>

export module mcpp.fallback.probe_sysroot;

import std;
import mcpp.xlings;
import mcpp.platform;
import mcpp.log;

export namespace mcpp::fallback {

// When GCC reports a baked "subos/default" sysroot that does not belong to
// THIS toolchain's home, remap it to the equivalent sysroot under the
// compiler's own xpkgs tree.
//
// The predicate used to be "does not exist", which is the wrong axis. A
// developer machine has more than one project-local `.xlings/subos/default`,
// and gcc's baked path can name one of them: it exists, so the remap was
// skipped and every build in an unrelated project inherited another
// project's sysroot. Measured -- a build in mcpp resolved
// `--sysroot=<other-repo>/.xlings/subos/default`.
//
// Existence says nothing about ownership. What matters is whether the path is
// under the same registry as the compiler, and a path that is not gets
// remapped whether or not something happens to be there.
std::optional<std::filesystem::path>
remap_xlings_baked_sysroot(std::string_view reportedPath,
                           const std::filesystem::path& compilerBin) {
    if (reportedPath.empty()) return std::nullopt;
    if (!reportedPath.ends_with("subos/default")) return std::nullopt;

    auto xpkgsOpt = mcpp::xlings::paths::xpkgs_from_compiler(compilerBin);
    if (xpkgsOpt) {
        // Owned by this toolchain's registry? Then it is the right answer.
        auto registry = xpkgsOpt->parent_path().parent_path();
        std::error_code ec;
        auto rel = std::filesystem::path(std::string(reportedPath))
                       .lexically_relative(registry);
        const bool inside = !rel.empty()
                         && rel.native().rfind("..", 0) != 0;
        if (inside && std::filesystem::exists(std::string(reportedPath), ec))
            return std::nullopt;
    }

    if (auto xpkgs = std::move(xpkgsOpt)) {
        // xpkgs is <registry>/data/xpkgs -> registry = xpkgs/../..
        auto registrySysroot = xpkgs->parent_path().parent_path()
                               / "subos" / "default";
        if (std::filesystem::exists(registrySysroot / "usr" / "include"))
            return registrySysroot;
    }
    return std::nullopt;
}

// Is this sysroot foreign -- neither this mcpp home's registry nor a tree
// belonging to the project being built?
//
// The hazard is specific and was measured: gcc bakes `--sysroot=<...>/.xlings/
// subos/default` as a STRING at build time, and a developer machine has many
// directories by that name. The baked one therefore frequently EXISTS while
// belonging to an unrelated checkout, and headers silently come from a tree
// this build never declared. remap_xlings_baked_sysroot repairs the case it
// can see; this predicate is what reports the case it cannot.
//
// Both anchors are required. Registry alone would flag every legitimate
// project-local tree; project alone would flag every payload sysroot.
bool sysroot_is_foreign(const std::filesystem::path& sysroot,
                        const std::filesystem::path& registryRoot,
                        const std::filesystem::path& projectRoot) {
    if (sysroot.empty()) return false;
    auto under = [&](const std::filesystem::path& anchor) {
        if (anchor.empty()) return false;
        auto rel = sysroot.lexically_relative(anchor);
        return !rel.empty() && rel.native().rfind("..", 0) != 0;
    };
    return !under(registryRoot) && !under(projectRoot);
}

// Parse a Clang .cfg file alongside the compiler binary for --sysroot=.
std::optional<std::filesystem::path>
parse_clang_cfg_sysroot(const std::filesystem::path& compilerBin) {
    auto stem = compilerBin.stem().string();
    auto cfgPath = compilerBin.parent_path() / (stem + ".cfg");
    if (!std::filesystem::exists(cfgPath)) return std::nullopt;

    std::ifstream ifs(cfgPath);
    std::string line;
    while (std::getline(ifs, line)) {
        constexpr std::string_view prefix = "--sysroot=";
        if (line.starts_with(prefix)) {
            // Trim whitespace
            auto val = std::string(line.substr(prefix.size()));
            while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' '))
                val.pop_back();
            while (!val.empty() && (val.front() == '\n' || val.front() == '\r' || val.front() == ' '))
                val.erase(val.begin());
            if (!val.empty() && std::filesystem::exists(val))
                return std::filesystem::path(val);
        }
    }
    return std::nullopt;
}

// macOS fallback: use xcrun to discover the SDK path.
std::optional<std::filesystem::path>
probe_macos_sdk_sysroot() {
    auto sdk = mcpp::platform::macos::sdk_path();
    if (sdk) {
        mcpp::log::verbose("probe", std::format("sysroot (macOS SDK): {}", sdk->string()));
    }
    return sdk;
}

} // namespace mcpp::fallback
