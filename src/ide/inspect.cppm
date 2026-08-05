export module mcpp.ide.inspect;

import std;
import mcpp.ide.model;
import mcpp.manifest;
import mcpp.project;

namespace mcpp::ide {

namespace {

struct Candidate {
    std::string name;
    std::string workspacePath;
    std::filesystem::path root;
    std::filesystem::path manifest;
    std::optional<WorkspaceMember> member;
    std::optional<Diagnostic> error;
};

std::filesystem::path absolute_path(std::filesystem::path path) {
    std::error_code ec;
    auto result = std::filesystem::absolute(path, ec);
    return (ec ? path : result).lexically_normal();
}

Diagnostic manifest_diagnostic(std::string code, Severity severity,
                              std::string message,
                              const std::filesystem::path& path,
                              std::optional<Range> range = std::nullopt) {
    return Diagnostic{std::move(code), severity, std::move(message), path,
                      std::move(range)};
}

std::optional<Range> error_range(const mcpp::manifest::ManifestError& error) {
    if (!error.line) return std::nullopt;
    return Range{{error.line, error.column}, {error.line, error.column}};
}

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    return absolute_path(path);
}

std::filesystem::path physical_path(const std::filesystem::path& path) {
    auto absolute = normalized_path(path);
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(absolute, ec);
    return ec ? absolute.lexically_normal() : normalized;
}

bool same_directory(const std::filesystem::path& left,
                    const std::filesystem::path& right) {
    std::error_code ec;
    const bool equivalent = std::filesystem::equivalent(left, right, ec);
    if (!ec) return equivalent;
    return physical_path(left) == physical_path(right);
}

struct ManifestRootSearch {
    std::optional<std::filesystem::path> root;
    std::optional<Diagnostic> error;
};

ManifestRootSearch find_manifest_root_read_only(std::filesystem::path start) {
    auto current = normalized_path(start);
    while (true) {
        const auto manifest = current / "mcpp.toml";
        std::error_code ec;
        const bool exists = std::filesystem::exists(manifest, ec);
        if (ec) {
            return {{}, manifest_diagnostic(
                "MCPP_IDE_MANIFEST_INVALID", Severity::Error,
                std::format("could not inspect mcpp.toml: {}", ec.message()), manifest)};
        }
        if (exists) return {current, {}};
        const auto parent = current.parent_path();
        if (parent == current) return {};
        current = parent;
    }
}

struct WorkspaceSearch {
    std::optional<std::filesystem::path> root;
    std::optional<mcpp::manifest::Manifest> manifest;
    std::optional<Diagnostic> error;
};

WorkspaceSearch find_workspace_read_only(const std::filesystem::path& memberRoot) {
    auto current = normalized_path(memberRoot).parent_path();
    while (true) {
        const auto manifestPath = current / "mcpp.toml";
        std::error_code ec;
        const bool exists = std::filesystem::exists(manifestPath, ec);
        if (ec) {
            return {{}, {}, manifest_diagnostic(
                "MCPP_IDE_MANIFEST_INVALID", Severity::Error,
                std::format("could not inspect ancestor mcpp.toml: {}", ec.message()),
                manifestPath)};
        }
        if (exists) {
            auto loaded = mcpp::manifest::load(manifestPath);
            if (!loaded) {
                const auto& error = loaded.error();
                return {{}, {}, manifest_diagnostic(
                    "MCPP_IDE_MANIFEST_INVALID", Severity::Error, error.message,
                    error.file, error_range(error))};
            }
            if (loaded->workspace.present) {
                for (const auto& declared : loaded->workspace.members) {
                    const auto candidate = normalized_path(
                        (current / declared).lexically_normal());
                    if (same_directory(candidate, memberRoot))
                        return {current, std::move(*loaded), {}};
                }
            }
        }
        const auto parent = current.parent_path();
        if (parent == current) return {};
        current = parent;
    }
}

WorkspaceMember describe_member(const mcpp::manifest::Manifest& manifest,
                                std::string workspacePath,
                                const std::filesystem::path& root) {
    WorkspaceMember result;
    result.name = manifest.package.name;
    result.version = manifest.package.version;
    result.workspacePath = std::move(workspacePath);
    result.root = root;
    result.manifest = manifest.sourcePath;
    for (const auto& target : manifest.targets) {
        DeclaredTarget out;
        out.name = target.name;
        if (target.kind == mcpp::manifest::Target::Library) {
            out.kind = "library";
        } else if (target.kind == mcpp::manifest::Target::SharedLibrary) {
            out.kind = "shared-library";
        } else if (target.kind == mcpp::manifest::Target::TestBinary) {
            out.kind = "test-binary";
            if (!target.main.empty()) out.main = target.main;
        } else {
            out.kind = "binary";
            if (!target.main.empty()) out.main = target.main;
        }
        result.targets.push_back(std::move(out));
    }
    return result;
}

bool matches_selector(const Candidate& candidate, std::string_view selector,
                      const std::filesystem::path&) {
    if (candidate.workspacePath == selector)
        return true;
    if (candidate.root.filename() == selector)
        return true;
    return false;
}

bool matches_workspace_path(const Candidate& candidate, std::string_view selector,
                            const std::filesystem::path&) {
    return candidate.workspacePath == selector;
}

} // namespace
} // namespace mcpp::ide

export namespace mcpp::ide {

WorkspaceInspection inspect_workspace(InspectRequest request) {
    WorkspaceInspection result;
    result.request = request;
    result.request.start = normalized_path(result.request.start);

    auto rootSearch = find_manifest_root_read_only(result.request.start);
    if (rootSearch.error) {
        result.workspaceRoot = rootSearch.error->path.parent_path();
        result.workspaceManifest = rootSearch.error->path;
        result.diagnostics.push_back(std::move(*rootSearch.error));
        return result;
    }
    if (!rootSearch.root) {
        result.workspaceRoot = result.request.start;
        result.workspaceManifest = result.workspaceRoot / "mcpp.toml";
        result.diagnostics.push_back(manifest_diagnostic(
            "MCPP_IDE_MANIFEST_NOT_FOUND", Severity::Error,
            "could not locate mcpp.toml", result.workspaceManifest));
        return result;
    }
    auto startRoot = normalized_path(*rootSearch.root);

    auto startManifest = mcpp::manifest::load(startRoot / "mcpp.toml");
    if (!startManifest) {
        result.workspaceRoot = startRoot;
        result.workspaceManifest = startRoot / "mcpp.toml";
        const auto& error = startManifest.error();
        result.diagnostics.push_back(manifest_diagnostic(
            "MCPP_IDE_MANIFEST_INVALID", Severity::Error, error.message,
            error.file, error_range(error)));
        return result;
    }

    std::filesystem::path workspaceRoot = startRoot;
    std::optional<mcpp::manifest::Manifest> workspaceManifest = *startManifest;
    std::filesystem::path currentMemberRoot;
    if (!startManifest->workspace.present) {
        auto found = find_workspace_read_only(startRoot);
        if (found.error) {
            result.workspaceRoot = found.error->path.parent_path();
            result.workspaceManifest = found.error->path;
            result.diagnostics.push_back(std::move(*found.error));
            return result;
        }
        if (found.root && found.manifest) {
            workspaceRoot = normalized_path(*found.root);
            currentMemberRoot = startRoot;
            workspaceManifest = std::move(*found.manifest);
        }
    }

    result.workspaceRoot = workspaceRoot;
    result.workspaceManifest = workspaceRoot / "mcpp.toml";

    std::vector<Candidate> candidates;
    auto add_manifest = [&](const mcpp::manifest::Manifest& manifest,
                            std::string workspacePath,
                            const std::filesystem::path& root) {
        auto member = describe_member(manifest, std::move(workspacePath), root);
        candidates.push_back(Candidate{member.name, member.workspacePath, root,
                                       member.manifest, std::move(member), std::nullopt});
    };

    if (workspaceManifest->workspace.present && !workspaceManifest->package.name.empty())
        add_manifest(*workspaceManifest, ".", workspaceRoot);
    if (!workspaceManifest->workspace.present)
        add_manifest(*workspaceManifest, ".", workspaceRoot);

    const auto& declared = workspaceManifest->workspace.members;
    for (const auto& workspacePath : declared) {
        const auto memberRoot = normalized_path(
            (workspaceRoot / workspacePath).lexically_normal());
        const auto manifestPath = memberRoot / "mcpp.toml";
        auto loaded = mcpp::manifest::load(manifestPath);
        if (loaded) {
            add_manifest(*loaded, workspacePath, memberRoot);
            continue;
        }
        std::error_code ec;
        const bool exists = std::filesystem::exists(manifestPath, ec);
        const auto code = exists || ec ? "MCPP_IDE_MEMBER_MANIFEST_INVALID"
                                       : "MCPP_IDE_MEMBER_MANIFEST_MISSING";
        const auto message = ec ? std::format("could not inspect member mcpp.toml: {}",
                                              ec.message())
                                : exists ? loaded.error().message
                                         : "member mcpp.toml not found";
        const auto range = exists && !ec ? error_range(loaded.error()) : std::nullopt;
        candidates.push_back(Candidate{
            manifestPath.filename().string(), workspacePath, memberRoot, manifestPath,
            std::nullopt,
            manifest_diagnostic(code, Severity::Error, message, manifestPath, range)});
    }

    std::vector<std::size_t> selected;
    auto select = [&](std::size_t index) { selected.push_back(index); };
    if (result.request.selectors.package) {
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (matches_workspace_path(candidates[i], *result.request.selectors.package,
                                       workspaceRoot)) {
                select(i);
                break;
            }
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (matches_selector(candidates[i], *result.request.selectors.package,
                                 workspaceRoot) && selected.empty())
                select(i);
        }
        if (selected.empty()) {
            result.diagnostics.push_back(manifest_diagnostic(
                "MCPP_IDE_WORKSPACE_MEMBER_NOT_FOUND", Severity::Error,
                std::format("workspace member '{}' not found",
                            *result.request.selectors.package),
                result.workspaceManifest));
            return result;
        }
    } else if (result.request.selectors.workspace
               || (workspaceManifest->workspace.present && currentMemberRoot.empty()
                   && workspaceManifest->package.name.empty())) {
        for (std::size_t i = 0; i < candidates.size(); ++i) select(i);
    } else if (workspaceManifest->workspace.present) {
        if (!currentMemberRoot.empty()) {
            for (std::size_t i = 0; i < candidates.size(); ++i)
                if (same_directory(candidates[i].root, currentMemberRoot)) select(i);
        } else if (!workspaceManifest->package.name.empty()) {
            for (std::size_t i = 0; i < candidates.size(); ++i)
                if (candidates[i].workspacePath == ".") select(i);
        }
    } else {
        if (!candidates.empty()) select(0);
    }

    std::set<std::size_t> selectedSet(selected.begin(), selected.end());
    bool selectedError = false;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto& candidate = candidates[i];
        if (candidate.member) {
            result.members.push_back(*candidate.member);
        } else {
            result.diagnostics.push_back(*candidate.error);
        }
        if (!candidate.member && selectedSet.contains(i))
            selectedError = true;
    }
    if (selected.empty()) {
        result.diagnostics.push_back(manifest_diagnostic(
            "MCPP_IDE_WORKSPACE_MEMBER_NOT_FOUND", Severity::Error,
            "no workspace member selected", result.workspaceManifest));
        return result;
    }
    if (selectedError) return result;

    bool hasStale = false;
    bool hasMissing = false;
    for (auto index : selected) {
        const auto& candidate = candidates[index];
        if (!candidate.member) continue;
        result.selectedMembers.push_back(candidate.name);
        const auto path = candidate.root / "compile_commands.json";
        std::error_code ec;
        const bool regular = std::filesystem::is_regular_file(path, ec);
        const auto state = regular ? ArtifactState::Stale : ArtifactState::Missing;
        result.compileCommands.push_back({candidate.name, path, state});
        hasStale = hasStale || regular;
        hasMissing = hasMissing || !regular;
    }
    if (hasMissing) result.diagnostics.push_back(manifest_diagnostic(
        "MCPP_IDE_ARTIFACTS_MISSING", Severity::Warning,
        "compile_commands.json is missing or could not be inspected for one or more selected members",
        result.workspaceManifest));
    if (hasStale) result.diagnostics.push_back(manifest_diagnostic(
        "MCPP_IDE_ARTIFACTS_UNVERIFIED", Severity::Warning,
        "compile_commands.json exists but was not verified",
        result.workspaceManifest));
    result.state = hasStale ? SnapshotState::Stale : SnapshotState::Partial;
    return result;
}

} // namespace mcpp::ide
