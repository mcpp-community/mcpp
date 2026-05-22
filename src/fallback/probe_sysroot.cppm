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

// When GCC reports a sysroot ending in "subos/default" that doesn't exist
// on the current machine (baked build-time path), remap it to the
// equivalent sysroot relative to the compiler's own xpkgs directory.
std::optional<std::filesystem::path>
remap_xlings_baked_sysroot(std::string_view reportedPath,
                           const std::filesystem::path& compilerBin) {
    if (reportedPath.empty()) return std::nullopt;
    if (!reportedPath.ends_with("subos/default")) return std::nullopt;
    if (std::filesystem::exists(std::string(reportedPath))) return std::nullopt;

    if (auto xpkgs = mcpp::xlings::paths::xpkgs_from_compiler(compilerBin)) {
        // xpkgs is <registry>/data/xpkgs -> registry = xpkgs/../..
        auto registrySysroot = xpkgs->parent_path().parent_path()
                               / "subos" / "default";
        if (std::filesystem::exists(registrySysroot / "usr" / "include"))
            return registrySysroot;
    }
    return std::nullopt;
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
