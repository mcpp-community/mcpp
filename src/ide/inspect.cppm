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
    return ec ? path : result;
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
                      const std::filesystem::path& workspaceRoot) {
    if (candidate.name == selector || candidate.workspacePath == selector)
        return true;
    if (candidate.root.filename() == selector)
        return true;
    std::error_code ec;
    auto relative = std::filesystem::relative(candidate.root, workspaceRoot, ec);
    return !ec && relative.generic_string() == selector;
}

bool matches_workspace_path(const Candidate& candidate, std::string_view selector,
                            const std::filesystem::path& workspaceRoot) {
    if (candidate.workspacePath == selector) return true;
    std::error_code ec;
    auto relative = std::filesystem::relative(candidate.root, workspaceRoot, ec);
    return !ec && relative.generic_string() == selector;
}

} // namespace
} // namespace mcpp::ide

export namespace mcpp::ide {

WorkspaceInspection inspect_workspace(InspectRequest request) {
    WorkspaceInspection result;
    result.request = request;
    result.request.start = absolute_path(std::move(result.request.start));

    auto startRoot = mcpp::project::find_manifest_root(result.request.start);
    if (!startRoot) {
        result.workspaceRoot = result.request.start;
        result.workspaceManifest = result.workspaceRoot / "mcpp.toml";
        result.diagnostics.push_back(manifest_diagnostic(
            "MCPP_IDE_MANIFEST_NOT_FOUND", Severity::Error,
            "could not locate mcpp.toml", result.workspaceManifest));
        return result;
    }
    *startRoot = absolute_path(*startRoot);

    auto startManifest = mcpp::manifest::load(*startRoot / "mcpp.toml");
    if (!startManifest) {
        result.workspaceRoot = *startRoot;
        result.workspaceManifest = *startRoot / "mcpp.toml";
        const auto& error = startManifest.error();
        result.diagnostics.push_back(manifest_diagnostic(
            "MCPP_IDE_MANIFEST_INVALID", Severity::Error, error.message,
            error.file, error_range(error)));
        return result;
    }

    std::filesystem::path workspaceRoot = *startRoot;
    std::optional<mcpp::manifest::Manifest> workspaceManifest = *startManifest;
    std::filesystem::path currentMemberRoot;
    if (!startManifest->workspace.present) {
        auto found = mcpp::project::find_workspace_root(*startRoot);
        if (!found.empty()) {
            workspaceRoot = absolute_path(found);
            currentMemberRoot = *startRoot;
            auto loaded = mcpp::manifest::load(workspaceRoot / "mcpp.toml");
            if (loaded) workspaceManifest = std::move(*loaded);
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
        const auto memberRoot = workspaceRoot / workspacePath;
        const auto manifestPath = memberRoot / "mcpp.toml";
        auto loaded = mcpp::manifest::load(manifestPath);
        if (loaded) {
            add_manifest(*loaded, workspacePath, memberRoot);
            continue;
        }
        std::error_code ec;
        const bool exists = std::filesystem::exists(manifestPath, ec);
        const auto code = ec ? "MCPP_IDE_MEMBER_MANIFEST_UNREADABLE"
                       : exists ? "MCPP_IDE_MEMBER_MANIFEST_INVALID"
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
                if (candidates[i].root == currentMemberRoot) select(i);
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
    bool hasUnreadable = false;
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
        hasUnreadable = hasUnreadable || static_cast<bool>(ec);
    }
    if (hasMissing) result.diagnostics.push_back(manifest_diagnostic(
        "MCPP_IDE_ARTIFACTS_MISSING", Severity::Warning,
        "compile_commands.json is missing for one or more selected members",
        result.workspaceManifest));
    if (hasStale) result.diagnostics.push_back(manifest_diagnostic(
        "MCPP_IDE_ARTIFACTS_UNVERIFIED", Severity::Warning,
        "compile_commands.json exists but was not verified",
        result.workspaceManifest));
    if (hasUnreadable) result.diagnostics.push_back(manifest_diagnostic(
        "MCPP_IDE_ARTIFACTS_UNREADABLE", Severity::Warning,
        "could not inspect compile_commands.json for one or more selected members",
        result.workspaceManifest));
    result.state = hasStale ? SnapshotState::Stale : SnapshotState::Partial;
    return result;
}

} // namespace mcpp::ide
