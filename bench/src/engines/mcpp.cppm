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
    explicit McppEngine(std::string program = "mcpp", std::string label = {},
                        std::map<std::string, std::string> env = {})
        : program_(std::move(program)), label_(std::move(label)), env_(std::move(env)) {}

    std::string_view name() const override {
        if (label_.empty()) label_ = discover_label();
        return label_;
    }

    Availability probe() const override {
        const auto v = version_string();
        if (v.empty())
            return {false, std::format("{} not runnable", program_)};
        // An xlings shim answers `--version` for a package it does not have,
        // prints "is not installed", and exits ZERO — see looks_uninstalled.
        if (looks_uninstalled(v)) return {false, uninstalled_reason(program_, v)};
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

        // Scoped, so a setting reaches THIS engine's child and is restored
        // before the next engine runs. A run that leaked one would silently
        // measure every later arm with the option on.
        std::vector<std::unique_ptr<platform::ScopedEnv>> scoped;
        scoped.reserve(env_.size() + 1);

        // ── THE COMPILER, and the one place mcpp used to escape the axis ─────
        //
        // mcpp resolves its own toolchain from the MEASURED PROJECT's manifest
        // and ignores the `--compiler` every other engine is handed. For the
        // generated fixture that is fine, because the harness writes that
        // manifest. For a REAL project it is not: the workloads are pinned
        // submodules whose `[toolchain]` says `gcc@16.1.0`, so on a clang cell
        // cmake and xmake were measured with clang while mcpp quietly used gcc —
        // a compiler-vs-compiler comparison wearing an engine-vs-engine label,
        // which is precisely what `resolve_cxx`'s fairness rule exists to stop.
        //
        // `--toolchain` is plumbed through MCPP_TOOLCHAIN, so the same mechanism
        // the bracket options use covers this too. An explicit engine option
        // wins, since that is the caller being specific on purpose.
        if (!env_.contains("MCPP_TOOLCHAIN")) {
            if (auto tc = toolchain_for(job.compiler); !tc.empty())
                scoped.push_back(std::make_unique<platform::ScopedEnv>("MCPP_TOOLCHAIN", tc));
        }
        for (const auto& [k, v] : env_)
            scoped.push_back(std::make_unique<platform::ScopedEnv>(k, v));
        return platform::run(argv, job.project_dir, job.log_path, job.timeout_s);
    }

    void clean(const Job& job) const override {
        // Artifacts only. ~/.mcpp holds the toolchain and the dependency cache;
        // removing those would measure provisioning, which is a different
        // question, and would make "cold" mean something else for this engine
        // than for the others.
        platform::remove_tree(job.project_dir / "target");
    }

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
    static std::string toolchain_for(std::string_view compiler) {
        if (compiler.empty() || compiler == "default" || compiler == "msvc") return {};
        if (toolchain::is_clang_request(compiler))
            return std::format("llvm@{}", toolchain::on_windows() ? toolchain::kLlvmWindows
                                                                  : toolchain::kLlvm);
        // A path that is neither clang nor gcc-shaped is the caller pinning
        // something the harness does not model; leave mcpp alone rather than
        // guess a family for it.
        if (compiler.find("gcc") != std::string_view::npos ||
            compiler.find("g++") != std::string_view::npos)
            return std::format("gcc@{}", toolchain::kGcc);
        return {};
    }

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
        // "mcpp@2026.8.13.1" — distinct per version, so two binaries never
        // collapse into one row of the result table.
        //
        // The env suffix is part of the identity for the same reason: the SAME
        // binary with `schedule=on` is a different engine to measure, and two
        // rows called `mcpp@2026.8.13.1` would be unreadable.
        std::string out = std::format("mcpp@{}", v.substr(sp + 1));
        for (const auto& [k, val] : env_) {
            std::string key = k;
            // `MCPP_BMI_SCHEDULE` -> `schedule`: the label is read by people.
            if (key.starts_with("MCPP_")) key.erase(0, 5);
            if (key.ends_with("_SCHEDULE") || key == "BMI_SCHEDULE") key = "schedule";
            for (char& c : key) c = static_cast<char>(std::tolower(c));
            out += std::format("+{}={}", key, val);
        }
        return out;
    }
};

export std::unique_ptr<Engine> make_mcpp(std::string program = "mcpp",
                                         std::string label = {},
                                         std::map<std::string, std::string> env = {}) {
    return std::make_unique<McppEngine>(std::move(program), std::move(label),
                                        std::move(env));
}

}  // namespace bench::engines
