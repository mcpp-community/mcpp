// bench.engines.cmake — CMake + Ninja.
//
// CMake has supported C++20 named modules since 3.28 (with Ninja >= 1.11), so it
// is the reference point for "the mainstream way to build modules today".
export module bench.engines.cmake;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;

namespace bench::engines {

class CMakeEngine : public Engine {
public:
    std::string_view name() const override { return "cmake"; }

    Availability probe() const override {
        auto a = probe_program("cmake", {"cmake", "--version"});
        if (!a.present) return a;
        // Ninja is not optional here: the Makefile generator cannot express
        // dyndep, so modules simply do not build. Reporting the real reason
        // beats a confusing configure failure later.
        if (!platform::have_program({"ninja", "--version"}))
            return {false, "cmake present but ninja is not; the Makefile generator cannot build C++20 modules"};
        return {true, "cmake + ninja"};
    }

    bool supports(Variant) const override { return true; }
    std::string unsupported_reason(Variant) const override { return {}; }

    platform::RunResult configure(const Job& job) const override {
        std::vector<std::string> argv{
            "cmake", "-S", job.project_dir.string(), "-B", job.build_dir.string(),
            "-G", "Ninja",
            std::format("-DCMAKE_BUILD_TYPE={}", job.profile == "debug" ? "Debug" : "Release"),
        };
        if (const auto cxx = resolve_cxx(job.compiler); !cxx.empty())
            argv.push_back(std::format("-DCMAKE_CXX_COMPILER={}", cxx));
        return platform::run(argv, {}, job.log_path);
    }

    platform::RunResult build(const Job& job) const override {
        std::vector<std::string> argv{"cmake", "--build", job.build_dir.string()};
        if (job.jobs > 0) { argv.push_back("-j"); argv.push_back(std::to_string(job.jobs)); }
        return platform::run(argv, {}, job.log_path);
    }

    // Artifacts only — the configure result lives in the same directory, so a
    // "cold" build here re-runs configure. That is declared in the bench README
    // rather than papered over: cmake genuinely cannot separate the two without
    // keeping a second cache.
    void clean(const Job& job) const override { platform::remove_tree(job.build_dir); }
};

export std::unique_ptr<Engine> make_cmake() { return std::make_unique<CMakeEngine>(); }

}  // namespace bench::engines
