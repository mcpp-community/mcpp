// mcpp.cli.common — shared CLI helpers: project/workspace discovery + fs utils
//
// Extracted verbatim from cli.cppm (cli modularization, see
// .agents/docs/2026-06-10-cli-modularization.md). Zero behavior change:
// bodies are byte-identical moves; only the surrounding module/namespace
// changed (mcpp::cli::detail -> mcpp::cli).

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.common;

import std;
import mcpp.manifest;
import mcpp.toolchain.detect;      // re-exports toolchain.model (Toolchain)
import mcpp.toolchain.fingerprint;

namespace mcpp::cli {

// Locate mcpp.toml by walking upward from cwd.
export std::optional<std::filesystem::path> find_manifest_root(std::filesystem::path start) {
    auto p = std::filesystem::absolute(start);
    while (true) {
        if (std::filesystem::exists(p / "mcpp.toml")) return p;
        auto parent = p.parent_path();
        if (parent == p) return std::nullopt;
        p = parent;
    }
}

// Find the workspace root by walking upward from a member directory.
// Returns empty if no workspace root found.
export std::filesystem::path find_workspace_root(const std::filesystem::path& memberRoot) {
    auto p = memberRoot.parent_path();
    while (true) {
        if (std::filesystem::exists(p / "mcpp.toml")) {
            auto m = mcpp::manifest::load(p / "mcpp.toml");
            if (m && m->workspace.present) {
                // Verify memberRoot is in members list
                auto rel = std::filesystem::relative(memberRoot, p);
                for (auto& member : m->workspace.members) {
                    if (rel == std::filesystem::path(member)) return p;
                }
            }
        }
        auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return {};
}

// Merge workspace.dependencies versions into a member's deps.
export void merge_workspace_deps(mcpp::manifest::Manifest& member,
                          const mcpp::manifest::Manifest& workspace) {
    auto merge_map = [&](std::map<std::string, mcpp::manifest::DependencySpec>& deps) {
        for (auto& [name, spec] : deps) {
            if (!spec.inheritWorkspace) continue;
            // Try exact key match first
            auto it = workspace.workspace.dependencies.find(name);
            if (it != workspace.workspace.dependencies.end()) {
                spec.version = it->second.version;
                spec.inheritWorkspace = false;
                continue;
            }
            // Try short name for default-ns deps
            auto shortIt = workspace.workspace.dependencies.find(spec.shortName);
            if (shortIt != workspace.workspace.dependencies.end()) {
                spec.version = shortIt->second.version;
                spec.inheritWorkspace = false;
            }
        }
    };
    merge_map(member.dependencies);
    merge_map(member.devDependencies);
    merge_map(member.buildDependencies);
}

export std::filesystem::path target_dir(const mcpp::toolchain::Toolchain& tc,
                                 const mcpp::toolchain::Fingerprint& fp,
                                 const std::filesystem::path& root)
{
    auto triple = tc.targetTriple.empty() ? std::string{"unknown"} : tc.targetTriple;
    return root / "target" / triple / fp.hex;
}

export std::uintmax_t dir_size(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return 0;
    std::uintmax_t total = 0;
    for (auto& e : std::filesystem::recursive_directory_iterator(p, ec)) {
        if (ec) break;
        std::error_code ec2;
        if (e.is_regular_file(ec2) && !ec2) {
            total += e.file_size(ec2);
        }
    }
    return total;
}

export std::string human_bytes(std::uintmax_t n) {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return std::format("{:.1f} {}", v, units[u]);
}

} // namespace mcpp::cli
