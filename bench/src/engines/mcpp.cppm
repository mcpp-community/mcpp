// bench.engines.mcpp — mcpp as a measured engine.
//
// PARAMETERISED BY BINARY, not by a simulated flag. `--engines mcpp=/path/to/A,
// mcpp=/path/to/B` registers two engines that differ only in which mcpp runs, and
// each labels itself with the version it reports. That is how "did this release
// get faster?" is answered: by running both releases, not by approximating one
// of them.
//
// An earlier revision had an `mcpp-opt` engine that set SOURCE_DATE_EPOCH around
// the build to emulate an optimisation. It was removed: emulating a change in
// the harness measures the harness's idea of the change, and it silently stops
// tracking the real implementation the moment the two diverge. Optimisations
// belong in mcpp; the bench measures binaries.
export module bench.engines.mcpp;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.toolchain;
import bench.engines.engine;

namespace bench::engines {

class McppEngine : public Engine {
public:
    // `program` may be a bare name resolved through PATH or an absolute path to
    // a specific build. `label` is what appears in results; empty means "ask the
    // binary", which is what makes a two-version comparison self-describing.
    //
    // `env` is how an OPT-IN BEHAVIOUR becomes a measurable engine.
    //
    // mcpp's split build schedule is `[build] bmi_schedule = "on"` in the measured
    // project's manifest, and it is opt-in until it has been verified on every
    // platform. That put the benchmark in an impossible position: the manifests
    // belong to the pinned workloads (one of them is someone else's project), so
    // the suite could not reach the single largest cold-build optimisation in
    // the release it was supposed to be measuring — and the table read "no
    // improvement on cold builds" for a change worth 2.29x.
    //
    // mcpp already exposes it as `MCPP_BMI_SCHEDULE`, so no flag had to be
    // invented; the harness only had to set it. Setting it per ENGINE rather
    // than per run is the point: both arms appear in the same report, against
    // the same baseline, on the same machine, in the same minute.
    explicit McppEngine(std::string program = "mcpp", std::string label = {}, std::map<std::string, std::string> env = {});

    std::string_view name() const override;

    Availability probe() const override;

    // mcpp compiles plain .cpp as readily as modules, and a Native project is
    // whatever it already is, so every variant is in scope.
    bool supports(Variant, std::string_view) const override;
    std::string unsupported_reason(Variant, std::string_view) const override;

    platform::RunResult configure(const Job&) const override;

    platform::RunResult build(const Job& job) const override;

    void clean(const Job& job) const override;

private:
    std::string                        program_;
    mutable std::string                label_;
    std::map<std::string, std::string> env_;

    // `--compiler` -> the mcpp toolchain spec that names the SAME payload every
    // other engine was handed. Empty for "default", where the project's own
    // manifest is the right answer and nothing should override it.
    //
    // The versions come from bench.toolchain, which is also where `payload:gcc`
    // is resolved — so the driver cmake is given and the toolchain mcpp is told
    // to use cannot name different versions.
    static std::string toolchain_for(std::string_view compiler);

    // `mcpp --version` prints "mcpp <version>". Empty means the binary could not
    // be run at all — which probe() reports as unavailable rather than failed.
    std::string version_string() const;

    std::string discover_label() const;
};

export std::unique_ptr<Engine> make_mcpp(std::string program = "mcpp",
                                         std::string label = {},
                                         std::map<std::string, std::string> env = {}) {
    return std::make_unique<McppEngine>(std::move(program), std::move(label),
                                        std::move(env));
}

}  // namespace bench::engines
