// mcpp.build.backend — abstract interface separating "what" from "how".

export module mcpp.build.backend;

import std;
import mcpp.build.plan;

export namespace mcpp::build {

enum class BackendKind { Ninja, Native };

struct BuildOptions {
    bool                        verbose       = false;
    bool                        dryRun       = false;
    std::size_t                 parallelJobs = 0;
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
};

struct BuildResult {
    int                                     exitCode = 0;
    std::vector<std::filesystem::path>      producedArtifacts;
    std::chrono::milliseconds               elapsed { 0 };
    std::size_t                             cacheHits   = 0;
    std::size_t                             cacheMisses = 0;
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
