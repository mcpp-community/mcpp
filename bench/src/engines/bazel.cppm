// bench.engines.bazel — Bazel.
//
// Headers variant only, for the same reason as meson: bazel's C++20 named-module
// support is not comparable to cmake/xmake today.
//
// Bazel is also the one engine whose "cold" is genuinely ambiguous. It keeps a
// persistent server and a large action cache outside the workspace, so
// `bazel clean` and `bazel clean --expunge` measure two very different things.
// This adapter uses the non-expunging form and says so in the result note,
// because expunging would also discard the downloaded toolchain — provisioning,
// not building.
export module bench.engines.bazel;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;

namespace bench::engines {

class BazelEngine : public Engine {
public:
    std::string_view name() const override { return "bazel"; }

    Availability probe() const override {
        auto a = probe_program("bazel", {"bazel", "--version"});
        // The note rides into every result cell, so the one asymmetry a reader
        // must know about is stated there rather than only in this source file:
        // bazel keeps a warm server and an action cache OUTSIDE the workspace,
        // and `clean` here is deliberately not `--expunge` (which would also
        // discard the toolchain and turn the measurement into provisioning).
        if (a.present) a.note = "bazel (cold excludes server start; clean is not --expunge)";
        return a;
    }

    bool supports(Variant v) const override { return v == Variant::Headers; }

    std::string unsupported_reason(Variant v) const override {
        if (v == Variant::Headers) return {};
        return "bazel's C++20 named-module support is not comparable to cmake/xmake; "
               "reporting a number here would misrepresent it";
    }

    platform::RunResult configure(const Job&) const override {
        return {0.0, 0};   // MODULE.bazel/BUILD are the configuration
    }

    platform::RunResult build(const Job& job) const override {
        std::vector<std::string> argv{"bazel", "build", "//..."};
        if (job.jobs > 0) argv.push_back(std::format("--jobs={}", job.jobs));
        argv.push_back(std::format("--compilation_mode={}",
                                   job.profile == "debug" ? "dbg" : "opt"));

        // Pin the driver like every other engine. This is also what makes bazel
        // WORK inside an xlings workspace: bazel autoconfigures its C++ toolchain
        // by probing `$CC -E -v` for builtin include dirs, and a workspace shim
        // reports directories that move with the workspace. The result is a
        // build that fails with "undeclared inclusion(s)" against perfectly real
        // system headers.
        //
        // BOTH mechanisms are needed and they are not interchangeable:
        //   CC in the environment  — read by the `local_config_cc` REPOSITORY
        //                            RULE when it autoconfigures the toolchain,
        //                            which is where the include dirs are decided
        //   --action_env=CC        — only reaches action execution, far too late
        //                            to affect that probe
        // Passing only the flag leaves the broken autoconfiguration in place;
        // that is exactly how this failed until the environment was set too.
        if (const auto cxx = resolve_cxx(job.compiler); !cxx.empty()) {
            argv.push_back(std::format("--action_env=CC={}", cxx));
            platform::ScopedEnv pin("CC", cxx);
            return platform::run(argv, job.project_dir, job.log_path);
        }
        return platform::run(argv, job.project_dir, job.log_path);
    }

    void clean(const Job& job) const override {
        // Deliberately NOT --expunge: that would drop the downloaded toolchain
        // and turn a build measurement into a provisioning measurement.
        platform::run({"bazel", "clean"}, job.project_dir, job.log_path);
        platform::remove_tree(job.build_dir);
    }
};

export std::unique_ptr<Engine> make_bazel() { return std::make_unique<BazelEngine>(); }

}  // namespace bench::engines
