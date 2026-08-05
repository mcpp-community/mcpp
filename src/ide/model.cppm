export module mcpp.ide.model;

import std;

export namespace mcpp::ide {

enum class SnapshotState {
    Partial,
    Stale,
    Unavailable,
    Ready,
};

enum class ArtifactState {
    Missing,
    Stale,
};

enum class Severity {
    Warning,
    Error,
};

std::string_view wire_name(SnapshotState state) {
    switch (state) {
    case SnapshotState::Partial: return "partial";
    case SnapshotState::Stale: return "stale";
    case SnapshotState::Unavailable: return "unavailable";
    case SnapshotState::Ready: return "ready";
    }
    return {};
}

std::string_view wire_name(ArtifactState state) {
    switch (state) {
    case ArtifactState::Missing: return "missing";
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
