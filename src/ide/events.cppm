export module mcpp.ide.events;

import std;
import mcpp.libs.json;

export namespace mcpp::ide {

enum class IdeEventType {
    OperationStarted,
    Progress,
    Diagnostic,
    SnapshotPublished,
    OperationFinished,
};

std::string_view wire_name(IdeEventType type) {
    switch (type) {
    case IdeEventType::OperationStarted: return "operation-started";
    case IdeEventType::Progress: return "progress";
    case IdeEventType::Diagnostic: return "diagnostic";
    case IdeEventType::SnapshotPublished: return "snapshot-published";
    case IdeEventType::OperationFinished: return "operation-finished";
    }
    return {};
}

struct IdeEvent {
    std::uint64_t seq = 0;
    IdeEventType type = IdeEventType::Progress;
    std::string operationId;
};

class NdjsonEventParser {
public:
    std::expected<std::optional<IdeEvent>, std::string> consume(std::string_view line) {
        if (line.find_first_not_of(" \t\r\n") == std::string_view::npos)
            return std::optional<IdeEvent>{};

        auto json = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        // schema 和 operationId 是事件关联契约的一部分；解析器不能把缺字段的
        // 行降级成合法事件，否则客户端无法隔离并发或迟到的操作。
        if (json.is_discarded() || !json.is_object()
            || !json.contains("schemaVersion") || !json["schemaVersion"].is_number_unsigned()
            || json["schemaVersion"].get<std::uint64_t>() != 1
            || !json.contains("seq") || !json["seq"].is_number_unsigned()
            || !json.contains("type") || !json["type"].is_string()) {
            return std::unexpected("MCPP_IDE_EVENT_INVALID");
        }
        if (!json.contains("operationId") || !json["operationId"].is_string()
            || json["operationId"].get_ref<const std::string&>().empty())
            return std::unexpected("MCPP_IDE_EVENT_INVALID");

        const auto seq = json["seq"].get<std::uint64_t>();
        if (seq <= lastSeq_)
            return std::unexpected("MCPP_IDE_EVENT_SEQUENCE_INVALID");

        const auto name = json["type"].get<std::string>();
        std::optional<IdeEventType> type;
        if (name == "operation-started") type = IdeEventType::OperationStarted;
        else if (name == "progress") type = IdeEventType::Progress;
        else if (name == "diagnostic") type = IdeEventType::Diagnostic;
        else if (name == "snapshot-published") type = IdeEventType::SnapshotPublished;
        else if (name == "operation-finished") type = IdeEventType::OperationFinished;
        else return std::unexpected("MCPP_IDE_EVENT_TYPE_UNKNOWN");

        const auto has_string = [&](std::string_view key) {
            return json.contains(key) && json[key].is_string()
                && !json[key].get_ref<const std::string&>().empty();
        };
        const auto has_unsigned = [&](std::string_view key) {
            return json.contains(key) && json[key].is_number_unsigned();
        };
        // 已知 type 还必须满足自己的 payload 契约；只验证 envelope 会让
        // 截断或字段拼写错误的事件悄悄进入客户端状态机。
        const bool payloadValid = [&] {
            switch (*type) {
            case IdeEventType::OperationStarted:
                return has_string("operation");
            case IdeEventType::Progress:
                return has_string("phase") && has_unsigned("completed")
                    && has_unsigned("total");
            case IdeEventType::Diagnostic:
                return json.contains("diagnostic") && json["diagnostic"].is_object();
            case IdeEventType::SnapshotPublished:
                return has_string("phase") && has_string("snapshotId")
                    && has_string("compileCommands");
            case IdeEventType::OperationFinished:
                return has_string("operation") && has_string("status");
            }
            return false;
        }();
        if (!payloadValid) return std::unexpected("MCPP_IDE_EVENT_INVALID");

        lastSeq_ = seq;
        return std::optional<IdeEvent>(IdeEvent{
            .seq = seq,
            .type = *type,
            .operationId = json["operationId"].get<std::string>(),
        });
    }

private:
    std::uint64_t lastSeq_ = 0;
};

} // namespace mcpp::ide
