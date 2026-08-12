// bench.engines.meson — Meson + Ninja.
//
// Meson is in the matrix for the HEADERS variant only. Its C++20 named-module
// support is not on par with CMake's or xmake's, and forcing a number out of it
// would be worse than reporting that it cannot play — see the suite's design
// note "不追求引擎功能对等". If upstream support lands, flipping `supports()`
// is the entire change needed here.
export module bench.engines.meson;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;

namespace bench::engines {

class MesonEngine : public Engine {
public:
    std::string_view name() const override { return "meson"; }

    Availability probe() const override {
        auto a = probe_program("meson", {"meson", "--version"});
        if (!a.present) return a;
        if (!platform::have_program({"ninja", "--version"}))
            return {false, "meson present but ninja is not"};
        return {true, std::format("meson {} + ninja", a.note)};
    }

    bool supports(Variant v, std::string_view) const override { return v == Variant::Headers; }

    std::string unsupported_reason(Variant v, std::string_view) const override {
        if (v == Variant::Headers) return {};
        // Measured, not assumed: meson 1.10.2 with clang 22 compiles main.cpp
        // without first building the interface unit and fails with
        // "fatal error: module 'fx.a' not found". There is no meson spelling
        // for "this source is a module interface".
        return "meson 1.10.2 does not build C++20 named modules (measured: "
               "\"module 'fx.a' not found\"; no attribute declares an interface unit)";
    }

    platform::RunResult configure(const Job& job) const override {
        std::vector<std::string> argv{
            "meson", "setup", job.build_dir.string(), job.project_dir.string(),
            std::format("--buildtype={}", job.profile == "debug" ? "debug" : "release"),
        };
        // meson reads the compiler from CXX at setup time and bakes it into the
        // build dir, so pinning it here fixes it for every later `meson compile`.
        if (const auto cxx = resolve_cxx(job.compiler); !cxx.empty()) {
            platform::ScopedEnv pin("CXX", cxx);
            return platform::run(argv, {}, job.log_path);
        }
        return platform::run(argv, {}, job.log_path);
    }

    platform::RunResult build(const Job& job) const override {
        std::vector<std::string> argv{"meson", "compile", "-C", job.build_dir.string()};
        if (job.jobs > 0) { argv.push_back("-j"); argv.push_back(std::to_string(job.jobs)); }
        return platform::run(argv, {}, job.log_path);
    }

    void clean(const Job& job) const override { platform::remove_tree(job.build_dir); }
};

export std::unique_ptr<Engine> make_meson() { return std::make_unique<MesonEngine>(); }

}  // namespace bench::engines
