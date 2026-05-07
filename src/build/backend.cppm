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
};

struct BuildResult {
    int                                     exitCode = 0;
    std::vector<std::filesystem::path>      producedArtifacts;
    std::chrono::milliseconds               elapsed { 0 };
    std::size_t                             cacheHits   = 0;
    std::size_t                             cacheMisses = 0;
};

struct BuildError {
    std::string                             message;
    std::optional<std::filesystem::path>    where;
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
