// bench.engines.mcpp — mcpp as a measured engine, including its optimised form.
//
// Two engines live here because they differ by CONFIGURATION, not by code:
// `mcpp` is the shipped behaviour, `mcpp-opt` additionally applies the
// optimisations validated in the 2026-08-12 analysis. Keeping them as two
// registry entries makes "before vs after" an ordinary axis of the matrix
// instead of a separate experiment run by hand.
export module bench.engines.mcpp;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;

namespace bench::engines {

class McppEngine : public Engine {
public:
    explicit McppEngine(bool optimised) : optimised_(optimised) {}

    std::string_view name() const override { return optimised_ ? "mcpp-opt" : "mcpp"; }

    Availability probe() const override {
        return probe_program("mcpp", {"mcpp", "--version"});
    }

    // mcpp compiles plain .cpp as readily as modules, so every fixture variant
    // is in scope.
    bool supports(Variant) const override { return true; }
    std::string unsupported_reason(Variant) const override { return {}; }

    platform::RunResult configure(const Job&) const override {
        return {0.0, 0};   // no separate configure step by design
    }

    platform::RunResult build(const Job& job) const override {
        const std::vector<std::string> argv{
            "mcpp", "build", job.profile == "debug" ? "--dev" : "--release"};

        // The optimisation under test is `SOURCE_DATE_EPOCH`. GCC stamps a wall
        // clock into every BMI, so mcpp's content-comparison cascade
        // suppression can never fire; pinning the epoch makes BMIs byte-stable
        // and it fires — measured 73.0 s -> 0.22 s on touch-hub.
        //
        // A FIXED constant, not "now": the whole point is that two builds a
        // minute apart produce identical bytes. The value is arbitrary but must
        // not change within a comparison.
        //
        // Scoped, so the variable never leaks into the next cell — an
        // unoptimised `mcpp` measurement running after an `mcpp-opt` one would
        // otherwise silently inherit the optimisation and the two would tie.
        if (optimised_) {
            platform::ScopedEnv epoch("SOURCE_DATE_EPOCH", "1700000000");
            return platform::run(argv, job.project_dir, job.log_path);
        }
        return platform::run(argv, job.project_dir, job.log_path);
    }

    void clean(const Job& job) const override {
        // Artifacts only. ~/.mcpp holds the toolchain and the dependency cache;
        // deleting those would measure provisioning, which is a different
        // question and would make "cold" mean something else for this engine
        // than for the others.
        platform::remove_tree(job.project_dir / "target");
    }

private:
    bool optimised_;
};

export std::unique_ptr<Engine> make_mcpp()     { return std::make_unique<McppEngine>(false); }
export std::unique_ptr<Engine> make_mcpp_opt() { return std::make_unique<McppEngine>(true); }

}  // namespace bench::engines
