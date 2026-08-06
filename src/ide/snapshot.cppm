export module mcpp.ide.snapshot;

import std;
import mcpp.ide.model;
import mcpp.libs.json;
import mcpp.version;

namespace mcpp::ide {

namespace {

using Json = nlohmann::ordered_json;

Json optional_string(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

Json string_array(const std::vector<std::string>& values) {
    auto result = Json::array();
    for (const auto& value : values) result.push_back(value);
    return result;
}

Json selectors_json(const Selectors& selectors) {
    auto result = Json::object();
    result["package"] = optional_string(selectors.package);
    result["workspace"] = selectors.workspace;
    result["profile"] = optional_string(selectors.profile);
    result["target"] = optional_string(selectors.target);
    result["features"] = string_array(selectors.features);
    result["capabilities"] = string_array(selectors.capabilities);
    result["includeDevDependencies"] = selectors.includeDevDependencies;
    return result;
}

Json targets_json(const std::vector<DeclaredTarget>& targets) {
    auto result = Json::array();
    for (const auto& target : targets) {
        auto item = Json::object();
        item["name"] = target.name;
        item["kind"] = target.kind;
        if (target.main) item["main"] = *target.main;
        result.push_back(std::move(item));
    }
    return result;
}

Json members_json(const std::vector<WorkspaceMember>& members) {
    auto result = Json::array();
    for (const auto& member : members) {
        auto item = Json::object();
        item["name"] = member.name;
        item["version"] = member.version;
        item["workspacePath"] = member.workspacePath;
        item["root"] = member.root.generic_string();
        item["manifest"] = member.manifest.generic_string();
        item["targets"] = targets_json(member.targets);
        result.push_back(std::move(item));
    }
    return result;
}

Json artifacts_json(const WorkspaceInspection& inspection) {
    auto compileCommands = Json::array();
    for (const auto& artifact : inspection.compileCommands) {
        auto item = Json::object();
        item["member"] = artifact.member;
        item["path"] = artifact.path.generic_string();
        item["state"] = std::string(wire_name(artifact.state));
        compileCommands.push_back(std::move(item));
    }
    auto result = Json::object();
    result["state"] = std::string(wire_name(inspection.state));
    result["compileCommands"] = std::move(compileCommands);
    return result;
}

Json position_json(const Position& position) {
    auto result = Json::object();
    result["line"] = position.line;
    result["column"] = position.column;
    return result;
}

Json diagnostics_json(const std::vector<Diagnostic>& diagnostics) {
    auto result = Json::array();
    for (const auto& diagnostic : diagnostics) {
        auto item = Json::object();
        item["code"] = diagnostic.code;
        item["severity"] = std::string(wire_name(diagnostic.severity));
        item["message"] = diagnostic.message;
        item["source"] = "mcpp";
        if (!diagnostic.path.empty()) item["path"] = diagnostic.path.generic_string();
        if (diagnostic.range) {
            auto range = Json::object();
            range["start"] = position_json(diagnostic.range->start);
            range["end"] = position_json(diagnostic.range->end);
            item["range"] = std::move(range);
        }
        result.push_back(std::move(item));
    }
    return result;
}

Json document_json(const WorkspaceInspection& inspection,
                   std::optional<std::string_view> snapshotId) {
    auto document = Json::object();
    document["schemaVersion"] = 1;
    document["kind"] = "mcpp.ide.snapshot";
    if (snapshotId) document["snapshotId"] = *snapshotId;
    document["state"] = std::string(wire_name(inspection.state));

    auto protocol = Json::object();
    protocol["min"] = 1;
    protocol["max"] = 1;
    auto mcpp = Json::object();
    mcpp["version"] = std::string(mcpp::MCPP_VERSION);
    mcpp["protocol"] = std::move(protocol);
    mcpp["capabilities"] = Json::array({
        "workspace-inspection",
        "manifest-diagnostics",
        "compile-commands-location",
    });
    document["mcpp"] = std::move(mcpp);

    auto request = Json::object();
    request["root"] = inspection.request.start.generic_string();
    request["selectors"] = selectors_json(inspection.request.selectors);
    request["mode"] = "read-only";
    document["request"] = std::move(request);

    auto workspace = Json::object();
    workspace["root"] = inspection.workspaceRoot.generic_string();
    workspace["manifest"] = inspection.workspaceManifest.generic_string();
    workspace["members"] = members_json(inspection.members);
    workspace["selectedMembers"] = string_array(inspection.selectedMembers);
    document["workspace"] = std::move(workspace);
    document["artifacts"] = artifacts_json(inspection);
    document["diagnostics"] = diagnostics_json(inspection.diagnostics);
    return document;
}

std::uint64_t fnv1a64(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

} // namespace mcpp::ide

export namespace mcpp::ide {

std::string snapshot_json(const WorkspaceInspection& inspection) {
    const auto canonical = document_json(inspection, std::nullopt).dump();
    const auto id = std::format("fnv1a64:{:016x}", fnv1a64(canonical));
    return document_json(inspection, id).dump(2) + "\n";
}

} // namespace mcpp::ide
