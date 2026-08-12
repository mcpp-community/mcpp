// bench.engines.xmake — xmake, which drives its own scheduler rather than ninja.
export module bench.engines.xmake;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;

namespace bench::engines {

class XmakeEngine : public Engine {
public:
    std::string_view name() const override { return "xmake"; }

    Availability probe() const override {
        return probe_program("xmake", {"xmake", "--version"});
    }

    bool supports(Variant, std::string_view) const override { return true; }
    std::string unsupported_reason(Variant, std::string_view) const override { return {}; }

    platform::RunResult configure(const Job& job) const override {
        std::vector<std::string> argv{
            "xmake", "f", "-y",
            // -P names the directory holding xmake.lua. For a fixture that is
            // the tree itself; for a real project the description lives beside
            // the bench and reaches back into the tree.
            "-P", job.buildfile_dir.string(),
            "-m", job.profile == "debug" ? "debug" : "release",
            "-o", job.build_dir.string(),
        };
        if (job.compiler == "clang") argv.push_back("--toolchain=llvm");
        //
        // The driver is pinned through CXX so every engine compiles with the
        // SAME binary; without it xmake resolves whatever `g++` means on this
        // host, and the comparison silently becomes compiler-vs-compiler.
        if (const auto cxx = resolve_cxx(job.compiler); !cxx.empty()) {
            platform::ScopedEnv pin("CXX", cxx);
            return platform::run(argv, job.project_dir, job.log_path);
        }
        return platform::run(argv, job.project_dir, job.log_path);
    }

    platform::RunResult build(const Job& job) const override {
        std::vector<std::string> argv{"xmake", "build", "-P", job.buildfile_dir.string()};
        if (job.jobs > 0) argv.push_back(std::format("-j{}", job.jobs));
        return platform::run(argv, job.project_dir, job.log_path);
    }

    // `.xmake/` holds the resolved configuration — the counterpart of a cmake
    // cache or mcpp's resolution.json. Removing it would measure toolchain
    // detection rather than the build, so only the artifact dir goes.
    void clean(const Job& job) const override { platform::remove_tree(job.build_dir); }
};

export std::unique_ptr<Engine> make_xmake() { return std::make_unique<XmakeEngine>(); }

}  // namespace bench::engines
