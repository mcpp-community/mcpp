// mcpp.hooks — running the project's `[hooks]` lifecycle commands.
//
// The CONFIGURATION is not parsed here: `[hooks]` is a section of mcpp.toml
// and mcpp.toml has one parser (mcpp.manifest). What lives here is the part
// that is policy rather than grammar — when a command runs, what a failure
// means, and what the user is told.

export module mcpp.hooks;

import std;
import mcpp.manifest;
import mcpp.platform.process;
import mcpp.ui;

export namespace mcpp::hooks {

enum class Event { BuildStart, BuildFailed, BuildFinished };

// Run the command this event names, if the project declared one.
//
// Returns whether the BUILD may keep its own result. False means a hook failed
// while `side_effect` was on, i.e. the failure is the build's now. A missing
// command, a disabled table, and a hook that failed under `side_effect = false`
// all return true — from the build's point of view nothing happened.
bool invoke(const mcpp::manifest::Hooks& hooks, Event event,
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

std::string_view event_command(const mcpp::manifest::Hooks& hooks, Event event) {
    switch (event) {
        case Event::BuildStart:    return hooks.buildStart;
        case Event::BuildFailed:   return hooks.buildFailed;
        case Event::BuildFinished: return hooks.buildFinished;
    }
    return {};
}

// One place decides what a hook failure costs, so `side_effect` cannot be
// honoured on one failure mode and forgotten on another. The message is
// identical either way — only its severity and the build's fate differ.
bool report_failure(const mcpp::manifest::Hooks& hooks, std::string message) {
    if (hooks.sideEffect) {
        mcpp::ui::error(message);
        return false;
    }
    mcpp::ui::warning(message);
    return true;
}

} // namespace

bool invoke(const mcpp::manifest::Hooks& hooks, Event event,
            const std::filesystem::path& projectRoot) {
    if (!hooks.enabled) return true;
    auto command = event_command(hooks, event);
    if (command.empty()) return true;

    bool timedOut = false;
    // The project root is passed to the launcher, not arranged with a chdir:
    // mcpp's working directory is process-wide state, and a hook is not
    // entitled to move it even briefly.
    int rc = mcpp::platform::process::run_shell_deadline(
        command, projectRoot.string(),
        std::chrono::seconds(hooks.timeoutSeconds), &timedOut);

    if (timedOut)
        return report_failure(hooks, std::format(
            "hook '{}' timed out after {}s", event_name(event),
            hooks.timeoutSeconds));
    if (rc != 0)
        return report_failure(hooks, std::format(
            "hook '{}' exited with status {}", event_name(event), rc));
    return true;
}

} // namespace mcpp::hooks
