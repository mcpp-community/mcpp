// mcpp.hooks — running the project's `[hooks]` lifecycle commands.
//
// ⚠️ EXPERIMENTAL. A hook cannot currently change whether a build succeeded:
// every failure is a warning, and `side_effect = true` is refused by the
// manifest parser. Only the ROOT project's hooks are ever run — a dependency's
// `[hooks]` is inert. See docs/05-mcpp-toml.md §2.16.
//
// The CONFIGURATION is not parsed here: `[hooks]` is a section of mcpp.toml
// and mcpp.toml has one parser (mcpp.manifest). What lives here is the part
// that is policy rather than grammar — when a command runs, what a failure
// means, and what the user is told.
//
// The model is one idea: a hook is a command mcpp OWNS FOR AN INTERVAL, and
// the event names the interval. `invoke` runs a SELF-CLOSING one (the interval
// ends when the command exits). `Span` owns a SPANNING one (`during_build`),
// whose interval is closed by the build. See
// .agents/docs/2026-08-30-project-build-hooks-owned-intervals.md.

export module mcpp.hooks;

import std;
import mcpp.manifest;
import mcpp.platform.process;
import mcpp.ui;

export namespace mcpp::hooks {

enum class Event { BuildStart, BuildFailed, BuildFinished };

// Run the command this event names, if the project declared one, and wait for
// it. The interval is self-closing: it ends when the command exits.
//
// Returns whether the BUILD may keep its own result. False means a hook failed
// while `side_effect` was on, i.e. the failure is the build's now. A missing
// command, a disabled table, and a hook that failed under `side_effect = false`
// all return true — from the build's point of view nothing happened.
bool invoke(const mcpp::manifest::Hooks& hooks, Event event,
            const std::filesystem::path& projectRoot);

// `during_build`: a command started before the build and stopped after it.
//
// RAII because the interval must close on every path out of the build,
// including the ones nobody writes down — an early return, an exception, a
// diagnostic that gives up. A background player that outlives its build is the
// failure this whole shape exists to prevent, so the destructor is the only
// place the stop can be promised from.
class Span {
public:
    Span() = default;
    Span(const mcpp::manifest::Hooks& hooks,
         const std::filesystem::path& projectRoot,
         bool inheritOutput);
    ~Span();
    Span(const Span&)            = delete;
    Span& operator=(const Span&) = delete;

    // Whether the BUILD may proceed. False means the command could not be
    // started and `side_effect` was on — the same verdict, in the same words,
    // that a failed `build_start` produces. Already reported.
    bool ok() const { return ok_; }

    // Close the interval and report what happened while it was open. Returns
    // whether the build may keep its result.
    //
    // Called explicitly BEFORE the terminal hook rather than left to the
    // destructor: a "build finished" sound playing over the background music
    // it was supposed to replace is the ordering this exists for.
    bool finish();

private:
    void close();

    mcpp::platform::process::BackgroundCommand child_;
    std::string           command_;
    std::filesystem::path root_;
    bool                  inheritOutput_ = false;
    bool                  declared_      = false;
    bool                  started_       = false;
    bool                  closed_        = false;
    bool                  sideEffect_    = false;
    bool                  ok_            = true;
    std::atomic<bool>     stop_{false};
    std::atomic<bool>     gaveUp_{false};
    std::thread           supervisor_;
};

} // namespace mcpp::hooks

namespace mcpp::hooks {

namespace {

namespace proc = mcpp::platform::process;

// `loop` restarts a command that ended before its interval did. Two bounds,
// fixed rather than configurable, because a knob whose wrong value is a spin
// is not a knob: a typo in the command (`play /nonexistant`) would otherwise
// restart thousands of times per second for the length of the build.
constexpr auto kRestartDelay   = std::chrono::milliseconds(250);
constexpr auto kTooShort       = std::chrono::milliseconds(1000);
constexpr int  kShortRunsBeforeGivingUp = 5;

// How long a stopped command is given to leave on its own before it is taken.
// A player asked to stop should get to close its audio device.
constexpr auto kStopGrace = std::chrono::milliseconds(2000);

std::string_view event_name(Event event) {
    switch (event) {
        case Event::BuildStart:    return "build_start";
        case Event::BuildFailed:   return "build_failed";
        case Event::BuildFinished: return "build_finished";
    }
    return "unknown";
}

const mcpp::manifest::HookCommand&
event_command(const mcpp::manifest::Hooks& hooks, Event event) {
    switch (event) {
        case Event::BuildFailed:   return hooks.buildFailed;
        case Event::BuildFinished: return hooks.buildFinished;
        case Event::BuildStart:    break;
    }
    return hooks.buildStart;
}

} // namespace

// One place decides what a hook failure costs, so `side_effect` cannot be
// honoured on one failure mode and forgotten on another — and there are now
// four of them (cannot start, non-zero, timed out, failed to stay up). The
// message is identical either way; only its severity and the build's fate
// differ.
//
// ⚠️ WHILE `[hooks]` IS EXPERIMENTAL, `sideEffect` IS ALWAYS FALSE — the
// manifest parser refuses `side_effect = true` (see
// modules/manifest/src/toml.cppm). The `true` branch below is therefore
// unreachable today ON PURPOSE: it is the behaviour the key will select when
// the feature is promoted, and keeping it here means promotion is a deletion
// in the parser rather than a reconstruction here.
bool report_hook_failure_flag(bool sideEffect, const std::string& message) {
    if (sideEffect) {
        mcpp::ui::error(message);
        return false;
    }
    mcpp::ui::warning(message);
    return true;
}

bool report_hook_failure(const mcpp::manifest::Hooks& hooks,
                         const std::string& message) {
    return report_hook_failure_flag(hooks.sideEffect, message);
}

bool invoke(const mcpp::manifest::Hooks& hooks, Event event,
            const std::filesystem::path& projectRoot) {
    if (!hooks.enabled) return true;
    auto const& hook = event_command(hooks, event);
    if (hook.empty()) return true;

    bool timedOut = false;
    // The project root is passed to the launcher, not arranged with a chdir:
    // mcpp's working directory is process-wide state, and a hook is not
    // entitled to move it even briefly.
    int rc = proc::run_shell_deadline(
        hook.cmd, projectRoot.string(),
        std::chrono::seconds(hooks.timeout_for(hook)), &timedOut);

    if (timedOut)
        return report_hook_failure(hooks, std::format(
            "hook '{}' timed out after {}s", event_name(event),
            hooks.timeout_for(hook)));
    if (rc != 0)
        return report_hook_failure(hooks, std::format(
            "hook '{}' exited with status {}", event_name(event), rc));
    return true;
}

Span::Span(const mcpp::manifest::Hooks& hooks,
           const std::filesystem::path& projectRoot,
           bool inheritOutput)
    : command_(hooks.duringBuild.cmd)
    , root_(projectRoot)
    , inheritOutput_(inheritOutput)
    , declared_(hooks.enabled && !hooks.duringBuild.empty())
    , sideEffect_(hooks.sideEffect)
{
    if (!declared_) return;

    child_ = proc::start_shell_background(command_, root_.string(),
                                          inheritOutput_);
    if (!child_.ok) {
        ok_ = report_hook_failure(hooks,
            "hook 'during_build' could not be started");
        return;
    }
    started_ = true;

    // Its own process group is what makes a whole-tree stop possible and what
    // stops the terminal's Ctrl-C from reaching it. Both halves are needed, so
    // the group is registered for the duration.
    proc::guard_background_on_signal(child_);

    if (!hooks.duringBuild.loop) return;

    // A supervisor exists ONLY for `loop`. Without it, a spanning hook is a
    // spawn and a stop, and no thread is created.
    supervisor_ = std::thread([this, runStarted = std::chrono::steady_clock::now()]
                              () mutable {
        int shortFailures = 0;
        while (!stop_.load()) {
            int code = 0;
            if (proc::background_running(child_, &code)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            // How long the run that JUST ENDED lasted. Measuring from the
            // restart instead would compare a spawn against itself and read
            // every run as instantaneous — which is a give-up on the fifth
            // restart of a perfectly healthy command.
            const auto ranFor = std::chrono::steady_clock::now() - runStarted;

            // Stop before restarting, not after: the run is over but whatever
            // it forked may not be, and the group is addressable only while
            // the unreaped leader still holds its id.
            proc::stop_background(child_, std::chrono::milliseconds(0));
            proc::clear_background_guard(child_);
            if (stop_.load()) return;

            // "Failed to stay up" is BOTH halves: short AND unsuccessful. A
            // command that finishes quickly and cleanly — one `ding.mp3`, an
            // `echo` — is doing exactly what `loop` was asked to repeat, and
            // counting it here would turn the feature into its own kill switch.
            if (code != 0 && ranFor < kTooShort) {
                if (++shortFailures >= kShortRunsBeforeGivingUp) {
                    gaveUp_.store(true);
                    return;
                }
            } else {
                shortFailures = 0;
            }

            std::this_thread::sleep_for(kRestartDelay);
            if (stop_.load()) return;

            runStarted = std::chrono::steady_clock::now();
            child_ = proc::start_shell_background(command_, root_.string(),
                                                  inheritOutput_);
            if (!child_.ok) {
                gaveUp_.store(true);
                return;
            }
            proc::guard_background_on_signal(child_);
        }
    });
}

void Span::close() {
    if (!declared_ || closed_) return;
    closed_ = true;
    stop_.store(true);
    if (supervisor_.joinable()) supervisor_.join();
    if (started_) {
        proc::clear_background_guard(child_);
        proc::stop_background(child_, kStopGrace);
    }
}

bool Span::finish() {
    const bool alreadyClosed = closed_;
    close();
    if (alreadyClosed || !started_) return ok_;
    if (!gaveUp_.load()) return ok_;
    // Reported once, here, rather than from the supervisor thread: ui writes
    // are not synchronised, and a thread printing into the middle of ninja's
    // output is the interleaving `during_build` discards its child's stdout to
    // avoid in the first place.
    return report_hook_failure_flag(sideEffect_, std::format(
        "hook 'during_build' failed to stay up: {} consecutive runs ended "
        "within {}ms", kShortRunsBeforeGivingUp, kTooShort.count()));
}

Span::~Span() { close(); }

} // namespace mcpp::hooks
