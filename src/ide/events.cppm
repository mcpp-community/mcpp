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
};

class NdjsonEventParser {
public:
    std::expected<std::optional<IdeEvent>, std::string> consume(std::string_view line) {
        if (line.find_first_not_of(" \t\r\n") == std::string_view::npos)
            return std::optional<IdeEvent>{};

        auto json = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (json.is_discarded() || !json.is_object()
            || !json.contains("seq") || !json["seq"].is_number_unsigned()
            || !json.contains("type") || !json["type"].is_string()) {
            return std::unexpected("MCPP_IDE_EVENT_INVALID");
        }

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

        lastSeq_ = seq;
        return std::optional<IdeEvent>(IdeEvent{.seq = seq, .type = *type});
    }

private:
    std::uint64_t lastSeq_ = 0;
};

} // namespace mcpp::ide
