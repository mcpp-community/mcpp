// bench.engines.mcpp — implementation.
//
// `module bench.engines.mcpp;` with no `export`: an implementation unit. The engine class
// stays declared in the interface and its bodies live here, so changing HOW
// this engine drives its tool does not change the BMI, and nothing that
// imports it has to be recompiled.
module bench.engines.mcpp;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.toolchain;
import bench.engines.engine;

namespace bench::engines {

McppEngine::McppEngine(std::string program, std::string label, std::map<std::string, std::string> env) : program_(std::move(program)), label_(std::move(label)), env_(std::move(env)) {}

std::string_view McppEngine::name() const {
        if (label_.empty()) label_ = discover_label();
        return label_;
    }

Availability McppEngine::probe() const {
        const auto v = version_string();
        if (v.empty())
            return {false, std::format("{} not runnable", program_)};
        // An xlings shim answers `--version` for a package it does not have,
        // prints "is not installed", and exits ZERO — see looks_uninstalled.
        if (looks_uninstalled(v)) return {false, uninstalled_reason(program_, v)};
        return {true, v};
    }

bool McppEngine::supports(Variant, std::string_view) const { return true; }

std::string McppEngine::unsupported_reason(Variant, std::string_view) const { return {}; }

platform::RunResult McppEngine::configure(const Job&) const {
        return {0.0, 0};   // no separate configure step by design
    }

platform::RunResult McppEngine::build(const Job& job) const {
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

void McppEngine::clean(const Job& job) const {
        // Artifacts only. ~/.mcpp holds the toolchain and the dependency cache;
        // removing those would measure provisioning, which is a different
        // question, and would make "cold" mean something else for this engine
        // than for the others.
        platform::remove_tree(job.project_dir / "target");
    }

std::string McppEngine::toolchain_for(std::string_view compiler) {
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

std::string McppEngine::version_string() const {
        const auto out = platform::run_capture({program_, "--version"});
        if (!out) return {};
        auto line = *out;
        if (const auto nl = line.find('\n'); nl != std::string::npos) line.resize(nl);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        return line;
    }

std::string McppEngine::discover_label() const {
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

}  // namespace bench::engines
