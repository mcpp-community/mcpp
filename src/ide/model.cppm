export module mcpp.ide.model;

import std;

export namespace mcpp::ide {

enum class IdePhase {
    Declared,
    Configured,
    Ready,
    Stale,
    Unavailable,
};

std::string_view wire_name(IdePhase phase) {
    switch (phase) {
    case IdePhase::Declared: return "declared";
    case IdePhase::Configured: return "configured";
    case IdePhase::Ready: return "ready";
    case IdePhase::Stale: return "stale";
    case IdePhase::Unavailable: return "unavailable";
    }
    return {};
}

enum class SnapshotState {
    Partial,
    Configured,
    Stale,
    Unavailable,
    Ready,
};

enum class ArtifactState {
    Missing,
    Configured,
    Stale,
};

enum class Severity {
    Warning,
    Error,
};

std::string_view wire_name(SnapshotState state) {
    switch (state) {
    case SnapshotState::Partial: return "partial";
    case SnapshotState::Configured: return "configured";
    case SnapshotState::Stale: return "stale";
    case SnapshotState::Unavailable: return "unavailable";
    case SnapshotState::Ready: return "ready";
    }
    return {};
}

std::string_view wire_name(ArtifactState state) {
    switch (state) {
    case ArtifactState::Missing: return "missing";
    case ArtifactState::Configured: return "configured";
    case ArtifactState::Stale: return "stale";
    }
    return {};
}

std::string_view wire_name(Severity severity) {
    switch (severity) {
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    }
    return {};
}

struct Position {
    std::size_t line = 0;
    std::size_t column = 0;
};

struct Range {
    Position start;
    Position end;
};

struct Diagnostic {
    std::string code;
    Severity severity = Severity::Error;
    std::string message;
    std::filesystem::path path;
    std::optional<Range> range;
};

struct Selectors {
    std::optional<std::string> package;
    bool workspace = false;
    std::optional<std::string> profile;
    std::optional<std::string> target;
    std::vector<std::string> features;
    std::vector<std::string> capabilities;
    bool includeDevDependencies = false;
};

// IDE 配置标识只包含用户选择和有效工具链，不包含时间戳或构建结果。
struct ConfigurationSelectors {
    std::optional<std::string> package;
    bool workspace = false;
    std::string profile;
    std::string target;
    std::vector<std::string> features;
    std::vector<std::string> capabilities;
    bool includeDevDependencies = false;
    std::string cacheMode = "global";
    std::string cppStandard = "c++23";
};

std::filesystem::path physical_project_root(const std::filesystem::path& root) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(root, ec);
    if (ec) absolute = root;
    auto physical = std::filesystem::weakly_canonical(absolute, ec);
    return (ec ? absolute : physical).lexically_normal();
}

std::optional<std::string>
canonical_member_selector(const std::filesystem::path& workspaceRoot,
                          const std::filesystem::path& projectRoot) {
    const auto workspace = physical_project_root(workspaceRoot);
    const auto project = physical_project_root(projectRoot);
    if (project == workspace) return std::nullopt;
    const auto relative = project.lexically_relative(workspace);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        return std::nullopt;
    return relative.generic_string();
}

std::string project_id(const std::filesystem::path& workspaceRoot) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : physical_project_root(workspaceRoot).generic_string()) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return std::format("project-fnv1a64:{:016x}", hash);
}

std::string configuration_id(const std::filesystem::path& workspaceRoot,
                             ConfigurationSelectors selectors,
                             std::string_view toolchainFingerprint) {
    auto normalize = [](std::vector<std::string>& values) {
        std::ranges::sort(values);
        values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    normalize(selectors.features);
    normalize(selectors.capabilities);

    std::string canonical;
    auto append = [&](std::string_view key, std::string_view value) {
        canonical += key;
        canonical.push_back('=');
        canonical += value;
        canonical.push_back('\x1f');
    };
    append("root", physical_project_root(workspaceRoot).generic_string());
    append("package", selectors.package.value_or(""));
    append("workspace", selectors.workspace ? "1" : "0");
    append("profile", selectors.profile);
    append("target", selectors.target);
    for (const auto& feature : selectors.features) append("feature", feature);
    for (const auto& capability : selectors.capabilities) append("capability", capability);
    append("dev", selectors.includeDevDependencies ? "1" : "0");
    append("cache", selectors.cacheMode);
    append("standard", selectors.cppStandard);
    append("toolchain", toolchainFingerprint);

    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : canonical) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return std::format("config-fnv1a64:{:016x}", hash);
}

struct InspectRequest {
    std::filesystem::path start;
    Selectors selectors;
};

struct DeclaredTarget {
    std::string name;
    std::string kind;
    std::optional<std::string> main;
};

struct WorkspaceMember {
    std::string name;
    std::string version;
    std::string workspacePath;
    std::filesystem::path root;
    std::filesystem::path manifest;
    std::vector<DeclaredTarget> targets;
};

struct CompileCommandsArtifact {
    std::string member;
    std::filesystem::path path;
    ArtifactState state = ArtifactState::Missing;
    std::optional<std::string> snapshotId;
    std::optional<std::string> configurationId;
};

struct WorkspaceInspection {
    SnapshotState state = SnapshotState::Unavailable;
    InspectRequest request;
    std::filesystem::path workspaceRoot;
    std::filesystem::path workspaceManifest;
    std::vector<WorkspaceMember> members;
    std::vector<std::string> selectedMembers;
    std::vector<CompileCommandsArtifact> compileCommands;
    std::vector<Diagnostic> diagnostics;
};

} // namespace mcpp::ide
