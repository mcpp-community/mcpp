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
import bench.engines.engine;

namespace bench::engines {

class McppEngine : public Engine {
public:
    // `program` may be a bare name resolved through PATH or an absolute path to
    // a specific build. `label` is what appears in results; empty means "ask the
    // binary", which is what makes a two-version comparison self-describing.
    explicit McppEngine(std::string program = "mcpp", std::string label = {})
        : program_(std::move(program)), label_(std::move(label)) {}

    std::string_view name() const override {
        if (label_.empty()) label_ = discover_label();
        return label_;
    }

    Availability probe() const override {
        const auto v = version_string();
        if (v.empty())
            return {false, std::format("{} not runnable", program_)};
        return {true, v};
    }

    // mcpp compiles plain .cpp as readily as modules, and a Native project is
    // whatever it already is, so every variant is in scope.
    bool supports(Variant, std::string_view) const override { return true; }
    std::string unsupported_reason(Variant, std::string_view) const override { return {}; }

    platform::RunResult configure(const Job&) const override {
        return {0.0, 0};   // no separate configure step by design
    }

    platform::RunResult build(const Job& job) const override {
        const std::vector<std::string> argv{
            program_, "build", job.profile == "debug" ? "--dev" : "--release"};
        return platform::run(argv, job.project_dir, job.log_path);
    }

    void clean(const Job& job) const override {
        // Artifacts only. ~/.mcpp holds the toolchain and the dependency cache;
        // removing those would measure provisioning, which is a different
        // question, and would make "cold" mean something else for this engine
        // than for the others.
        platform::remove_tree(job.project_dir / "target");
    }

private:
    std::string         program_;
    mutable std::string label_;

    // `mcpp --version` prints "mcpp <version>". Empty means the binary could not
    // be run at all — which probe() reports as unavailable rather than failed.
    std::string version_string() const {
        const auto out = platform::run_capture({program_, "--version"});
        if (!out) return {};
        auto line = *out;
        if (const auto nl = line.find('\n'); nl != std::string::npos) line.resize(nl);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        return line;
    }

    std::string discover_label() const {
        const auto v = version_string();          // e.g. "mcpp 2026.8.12.1"
        if (v.empty()) return "mcpp";
        const auto sp = v.rfind(' ');
        if (sp == std::string::npos) return "mcpp";
        // "mcpp@2026.8.12.1" — distinct per version, so two binaries never
        // collapse into one row of the result table.
        return std::format("mcpp@{}", v.substr(sp + 1));
    }
};

export std::unique_ptr<Engine> make_mcpp(std::string program = "mcpp",
                                         std::string label = {}) {
    return std::make_unique<McppEngine>(std::move(program), std::move(label));
}

}  // namespace bench::engines
