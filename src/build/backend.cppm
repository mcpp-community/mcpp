// mcpp.build.backend — abstract interface separating "what" from "how".

export module mcpp.build.backend;

import std;
import mcpp.build.plan;
import mcpp.build.progress;

export namespace mcpp::build {

enum class BackendKind { Ninja, Native };

struct BuildOptions {
    bool                        verbose       = false;
    bool                        dryRun       = false;
    bool                        requireCompileDatabase = false;
    // Explicit ninja goal targets (LinkUnit::output paths, relative to the
    // plan's outputDir). Empty = build the full plan (default behavior).
    std::vector<std::string>    ninjaTargets;
    // Keep building unaffected goals after a failure (ninja -k 0). Used by
    // `mcpp test` to pre-build all test goals in one parallel pass.
    bool                        keepGoing = false;
    // Wall-clock ceiling for THIS ninja invocation, in seconds. 0 = no limit.
    //
    // `mcpp test --timeout` only ever bounded the test binary's *run*; the
    // three build drives around it had no deadline at all, so a build that
    // never finishes (measured: 14 executables linking against a prebuilt
    // JavaScriptCore on macOS, >44 min and still going) could only be stopped
    // by the CI job timeout — which kills the process and takes its unflushed
    // output with it. This is the knob that turns that into an attributable
    // failure. POSIX only: the deadline runner has no kill-by-handle path on
    // Windows (see mcpp.platform.process), where the value is ignored.
    unsigned                    buildTimeoutSecs = 0;
    // The report of this build directory (build progress design 2026-09-29).
    // Set, ninja runs without `--quiet` and is read as it runs: the step
    // record is written, the status lines and ninja's log feed the report,
    // and a failed step's diagnostics are written when it fails. Null, the
    // backend applies `report`.
    mcpp::build::progress::Build* progress = nullptr;
    // Who states this drive when `progress` is null (pack drive and selection
    // design 2026-10-01, A2). `Region`: the backend reports it whenever the
    // command opened the progress region and is not quiet, which is how
    // `mcpp pack` came to build for six minutes under `Planning` (#753): the
    // default is the reported form, so a caller that says nothing is
    // reported. `Caller`: the caller writes its own lines, as `mcpp test`
    // does for each test's build.
    //
    // The job count is not an option. Every drive takes it from the plan
    // (`BuildPlan::scheduleNinjaJobs`), where `[build] jobs`, `--jobs` and
    // `MCPP_JOBS` were resolved once; it was an option set by one of ten
    // callers, and `test` and `pack` ran ninja's default instead (A1).
    enum class Report { Region, Caller };
    Report                      report = Report::Region;
};

struct BuildResult {
    int                                     exitCode = 0;
    std::vector<std::filesystem::path>      producedArtifacts;
    std::chrono::milliseconds               elapsed { 0 };
    std::size_t                             cacheHits   = 0;
    std::size_t                             cacheMisses = 0;
    std::size_t                             compileCommands = 0;
    std::string                             ninjaProgram;     // P0: cached for fast-path rebuilds
    std::string                             runtimeEnvKey;    // cached for fast-path rebuilds
    std::string                             runtimeEnvValue;  // cached for fast-path rebuilds
};

struct BuildError {
    std::string                             message;
    std::optional<std::filesystem::path>    where;
    std::string                             diagnosticOutput;
    // Set when the drive was killed by buildTimeoutSecs rather than failing to
    // compile. A flag, not a message prefix: `mcpp test` reports a timed-out
    // compile differently from a broken one, and matching on prose is how that
    // distinction silently rots.
    bool                                    timedOut = false;
    // `message` and the failed steps' diagnostics were written as the steps
    // failed; `diagnosticOutput` holds only what follows them (the advice
    // that reads the whole output).
    bool                                    reported = false;
};

struct Backend {
    virtual ~Backend() = default;
    virtual std::string_view name() const = 0;

    virtual std::expected<BuildResult, BuildError>
        build(const BuildPlan& plan, const BuildOptions& opts) = 0;

    virtual std::expected<std::vector<std::filesystem::path>, BuildError>
        stale_units(const BuildPlan&) {
        return std::unexpected(BuildError{"stale_units not implemented for this backend", std::nullopt});
    }
};

// Factories live in their respective implementation modules; the CLI
// dispatches at the call site. This avoids a backend.cppm → ninja.cppm
// import which would otherwise create a circular layering.

} // namespace mcpp::build
