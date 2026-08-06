module;
#include <cstdio>

export module mcpp.cli.cmd_ide;

import std;
import mcpplibs.cmdline;
import mcpp.ide.configure;
import mcpp.ide.inspect;
import mcpp.ide.model;
import mcpp.ide.snapshot;

namespace mcpp::cli {

namespace {

std::vector<std::string> split_csv(std::string_view value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(',', start);
        const auto item = value.substr(
            start, end == std::string_view::npos ? value.size() - start : end - start);
        if (!item.empty()) result.emplace_back(item);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

mcpp::ide::Selectors selectors_from(const mcpplibs::cmdline::ParsedArgs& parsed) {
    mcpp::ide::Selectors selectors;
    if (auto value = parsed.value("package")) selectors.package = *value;
    selectors.workspace = parsed.is_flag_set("workspace");
    if (auto value = parsed.value("profile")) selectors.profile = *value;
    if (auto value = parsed.value("target")) selectors.target = *value;
    if (auto value = parsed.value("features")) selectors.features = split_csv(*value);
    if (auto value = parsed.value("cap")) selectors.capabilities = split_csv(*value);
    selectors.includeDevDependencies = parsed.is_flag_set("include-dev-dependencies");
    return selectors;
}

} // namespace

export int cmd_ide_snapshot(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::error_code ec;
    auto start = std::filesystem::current_path(ec);
    if (ec) {
        start = ".";
        ec.clear();
    }
    auto absolute = std::filesystem::absolute(start, ec);
    if (!ec) start = std::move(absolute);
    start = start.lexically_normal();

    mcpp::ide::InspectRequest request{
        .start = std::move(start),
        .selectors = selectors_from(parsed),
    };
    const auto format = parsed.value("format").value_or("json");
    mcpp::ide::WorkspaceInspection inspection;
    if (format == "json") {
        inspection = mcpp::ide::inspect_workspace(std::move(request));
    } else {
        inspection.request = std::move(request);
        inspection.workspaceRoot = inspection.request.start;
        inspection.workspaceManifest = inspection.workspaceRoot / "mcpp.toml";
        inspection.diagnostics.push_back({
            .code = "MCPP_IDE_UNSUPPORTED_FORMAT",
            .severity = mcpp::ide::Severity::Error,
            .message = std::format("unsupported IDE snapshot format '{}'; expected json",
                                   format),
        });
    }
    std::print("{}", mcpp::ide::snapshot_json(inspection));
    return inspection.state == mcpp::ide::SnapshotState::Unavailable ? 3 : 0;
}

export int cmd_ide_configure(const mcpplibs::cmdline::ParsedArgs& parsed) {
    const auto format = parsed.value("format").value_or("ndjson");
    if (format != "ndjson") {
        // IDE 客户端依赖逐行 JSON；拒绝未知格式，避免先执行 prepare
        // 再发现无法解析，或在错误请求下意外发布兼容 CDB。
        std::println(stderr, "error: unsupported IDE configure format '{}'; expected ndjson",
                     format);
        return 2;
    }
    std::error_code ec;
    auto start = std::filesystem::current_path(ec);
    if (ec) start = ".";
    auto absolute = std::filesystem::absolute(start, ec);
    if (!ec) start = std::move(absolute);

    mcpp::ide::ConfigureRequest request{
        .start = start.lexically_normal(),
        .selectors = selectors_from(parsed),
    };
    const auto result = mcpp::ide::configure_project(request);
    if (!result) {
        std::println("{}", mcpp::ide::configure_error_event(result.error()));
        return 3;
    }
    for (const auto& line : mcpp::ide::configure_events(*result))
        std::println("{}", line);
    return 0;
}

} // namespace mcpp::cli
