// mcpp.project — project/workspace location + workspace-dependency merging.
//
// Shared by the CLI layer and the pm subsystem (which previously kept a
// private copy of find_manifest_root to avoid importing mcpp.cli).
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.project;

import std;
import mcpp.manifest;

namespace mcpp::project {

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

// Resolve which member directory a workspace command acts on, for the
// single-member case. Shares the match rule (basename OR member path) with
// prepare_build's member switch, so `build -p X` and `test -p X` agree.
// Returns:
//   - the member dir   when `package_filter` names a member,
//   - empty path       when no switch applies (not a workspace, or a rooted
//                      workspace with no filter → act on the root package),
//   - error            when the filter names an unknown member, or a *virtual*
//                      workspace is addressed with no filter (the caller must
//                      pick a member with -p or fan out with --workspace).
export std::expected<std::filesystem::path, std::string>
resolve_member_dir(const mcpp::manifest::Manifest& rootManifest,
                   const std::filesystem::path& rootDir,
                   std::string_view package_filter) {
    if (!rootManifest.workspace.present) return std::filesystem::path{};
    if (!package_filter.empty()) {
        for (auto& mp : rootManifest.workspace.members) {
            auto basename = std::filesystem::path(mp).filename().string();
            if (basename == package_filter || mp == package_filter)
                return rootDir / mp;
        }
        return std::unexpected(std::format(
            "workspace member '{}' not found in [workspace].members", package_filter));
    }
    if (rootManifest.package.name.empty()) {
        return std::unexpected(std::string(
            "virtual workspace: specify -p <member> or --workspace"));
    }
    return std::filesystem::path{};  // rooted workspace, no filter → root package
}

} // namespace mcpp::project
