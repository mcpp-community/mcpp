// mcpp.hooks — project-level build lifecycle commands from mcpp.toml.

export module mcpp.hooks;

import std;
import mcpp.libs.toml;
import mcpp.platform.process;
import mcpp.ui;

export namespace mcpp::hooks {

enum class Event { BuildStart, BuildFailed, BuildFinished };

struct Config {
    std::string buildStart;
    std::string buildFailed;
    std::string buildFinished;
    int timeoutSeconds = 10;
    bool enabled = true;
    bool sideEffect = true;
};

std::expected<Config, std::string>
load(const std::filesystem::path& manifestPath);

bool active(const Config& config);

// True means the build may continue/keep its original result. False means a
// hook failed with side_effect=true.
bool invoke(const Config& config, Event event,
            const std::filesystem::path& projectRoot);

} // namespace mcpp::hooks

namespace mcpp::hooks {

namespace {

std::string_view event_name(Event event) {
    switch (event) {
        case Event::BuildStart:    return "build_start";
        case Event::BuildFailed:   return "build_failed";
        case Event::BuildFinished: return "build_finished";
    }
    return "unknown";
}

std::string_view event_command(const Config& config, Event event) {
    switch (event) {
        case Event::BuildStart:    return config.buildStart;
        case Event::BuildFailed:   return config.buildFailed;
        case Event::BuildFinished: return config.buildFinished;
    }
    return {};
}

bool report_failure(const Config& config, std::string message) {
    if (config.sideEffect) {
        mcpp::ui::error(message);
        return false;
    }
    mcpp::ui::warning(message);
    return true;
}

} // namespace

std::expected<Config, std::string>
load(const std::filesystem::path& manifestPath) {
    namespace toml = mcpp::libs::toml;

    auto doc = toml::parse_file(manifestPath);
    if (!doc) {
        auto const& e = doc.error();
        if (e.where.line != 0)
            return std::unexpected(std::format(
                "mcpp.toml:{}:{}: {}", e.where.line, e.where.column, e.message));
        return std::unexpected(e.message);
    }

    Config config;
    auto* hooksValue = doc->get("hooks");
    if (!hooksValue) return config;
    if (!hooksValue->is_table())
        return std::unexpected("[hooks] must be a table");

    auto const& table = hooksValue->as_table();
    static constexpr std::string_view known[] = {
        "build_start", "build_failed", "build_finished",
        "timeout_seconds", "enabled", "side_effect",
    };
    for (auto const& [key, _] : table) {
        if (std::ranges::find(known, key) == std::end(known))
            return std::unexpected(std::format("[hooks] has unknown key '{}'", key));
    }

    std::string error;
    auto read_command = [&](std::string_view key, std::string& out) {
        auto it = table.find(key);
        if (it == table.end()) return true;
        if (!it->second.is_string() || it->second.as_string().empty()) {
            error = std::format("[hooks].{} must be a non-empty string", key);
            return false;
        }
        out = it->second.as_string();
        return true;
    };
    if (!read_command("build_start", config.buildStart)
        || !read_command("build_failed", config.buildFailed)
        || !read_command("build_finished", config.buildFinished))
        return std::unexpected(std::move(error));

    if (auto it = table.find("timeout_seconds"); it != table.end()) {
        if (!it->second.is_int() || it->second.as_int() <= 0
            || it->second.as_int() > std::numeric_limits<int>::max())
            return std::unexpected(
                "[hooks].timeout_seconds must be a positive integer");
        config.timeoutSeconds = static_cast<int>(it->second.as_int());
    }

    auto read_bool = [&](std::string_view key, bool& out) {
        auto it = table.find(key);
        if (it == table.end()) return true;
        if (!it->second.is_bool()) {
            error = std::format("[hooks].{} must be a boolean", key);
            return false;
        }
        out = it->second.as_bool();
        return true;
    };
    if (!read_bool("enabled", config.enabled)
        || !read_bool("side_effect", config.sideEffect))
        return std::unexpected(std::move(error));

    return config;
}

bool active(const Config& config) {
    return config.enabled
        && (!config.buildStart.empty() || !config.buildFailed.empty()
            || !config.buildFinished.empty());
}

bool invoke(const Config& config, Event event,
            const std::filesystem::path& projectRoot) {
    if (!config.enabled) return true;
    auto command = event_command(config, event);
    if (command.empty()) return true;

    std::error_code ec;
    auto previous = std::filesystem::current_path(ec);
    if (ec)
        return report_failure(config, std::format(
            "hook '{}' could not read the current directory: {}",
            event_name(event), ec.message()));

    // ponytail: hooks are synchronous; a scoped cwd is the smallest portable
    // way to honour the project-root contract. Pass cwd through the process
    // API if hook execution ever becomes concurrent.
    std::filesystem::current_path(projectRoot, ec);
    if (ec)
        return report_failure(config, std::format(
            "hook '{}' could not enter the project root: {}",
            event_name(event), ec.message()));

    struct RestorePath {
        std::filesystem::path path;
        bool active = true;
        ~RestorePath() {
            if (!active) return;
            std::error_code ignored;
            std::filesystem::current_path(path, ignored);
        }
    } restore{previous};

    bool timedOut = false;
    int rc = mcpp::platform::process::run_shell_deadline(
        command, std::chrono::seconds(config.timeoutSeconds), &timedOut);

    std::error_code restoreError;
    std::filesystem::current_path(previous, restoreError);
    restore.active = false;
    if (restoreError) {
        mcpp::ui::error(std::format(
            "hook '{}' could not restore the working directory: {}",
            event_name(event), restoreError.message()));
        return false;
    }

    if (timedOut)
        return report_failure(config, std::format(
            "hook '{}' timed out after {}s",
            event_name(event), config.timeoutSeconds));
    if (rc != 0)
        return report_failure(config, std::format(
            "hook '{}' exited with status {}", event_name(event), rc));
    return true;
}

} // namespace mcpp::hooks
