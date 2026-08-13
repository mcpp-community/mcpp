// bench.engines.bazel — Bazel.
//
// Bazel DOES build C++20 named modules — a claim worth stating precisely, because
// the obvious guess is wrong in both directions. Measured on bazel 9.2.0 with
// rules_cc 0.2.22:
//
//   * the `module_interfaces` attribute exists on cc_binary/cc_library, and
//   * it needs BOTH `--experimental_cpp_modules` and `--features=cpp_modules`
//     (each flag's absence produces a different, explicit error), and
//   * with clang it builds and runs; with GCC it dies in bazel's own scanner:
//         aggregate-ddi failed: ... what(): Invalid JSON string
//     i.e. bazel's ddi aggregator cannot parse GCC's P1689 output.
//
// So module support here is CONDITIONAL ON THE COMPILER, which is why supports()
// takes one.
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
        if (a.present)
            a.note = std::format("{} (cold excludes server start; clean is not --expunge)", a.note);
        return a;
    }

    bool supports(Variant v, std::string_view compiler) const override {
        if (v == Variant::Headers) return true;
        return is_clang(compiler);
    }

    std::string unsupported_reason(Variant v, std::string_view compiler) const override {
        if (v == Variant::Headers || is_clang(compiler)) return {};
        return "bazel 9.2 builds C++20 modules with clang, but its ddi aggregator "
               "cannot parse GCC's P1689 output (aggregate-ddi: \"Invalid JSON string\"); "
               "re-run with --compiler <clang++> to measure this cell";
    }

    platform::RunResult configure(const Job&) const override {
        return {0.0, 0};   // MODULE.bazel/BUILD are the configuration
    }

    // `bazel build //...` over a package with no rules is a SUCCESS that compiles
    // nothing, in about 0.2s. Ask bazel itself what it is about to build rather
    // than reading the BUILD file here — a hand-rolled rule detector would be one
    // more parser to keep in step with the file it parses.
    std::string unbuildable_reason(const Job& job) const override {
        platform::RunResult r;
        const auto out = platform::run_capture(
            {"bazel", "query", "kind(rule, //...)", "--noshow_progress"},
            job.buildfile_dir, &r);
        // A query that ERRORS is not "no targets" — a broken query is a real
        // failure, and it belongs in the build where the log gets reported.
        if (!out || !r.ok()) return {};
        // The capture is stdout+stderr combined, so presence is tested on the one
        // token bazel never prints by accident: a target label at line start.
        for (std::string_view rest = *out; !rest.empty();) {
            const auto nl   = rest.find('\n');
            const auto line = rest.substr(0, nl);
            if (line.starts_with("//")) return {};
            if (nl == std::string_view::npos) break;
            rest.remove_prefix(nl + 1);
        }
        return std::format(
            "{}/BUILD.bazel declares no rules, so `bazel build //...` would exit 0 "
            "having compiled nothing and report a ~0.2s 'build'. bazel cannot glob "
            "sources from outside its workspace and has no spelling for `import std;`",
            job.buildfile_dir.filename().string());
    }

    platform::RunResult build(const Job& job) const override {
        std::vector<std::string> argv{"bazel", "build", "//..."};
        if (job.jobs > 0) argv.push_back(std::format("--jobs={}", job.jobs));

        // Applied to EVERY variant, not just the module ones, so bazel's own
        // headers-vs-modules rows stay comparable to each other.
        //
        // It is also load-bearing for modules: cc_binary registers the ddi
        // aggregation action for both the PIC and the non-PIC object sets, but
        // names its output `<target>.CXXModules.json` without a pic suffix, so
        // analysis dies before a single file is compiled:
        //     Attempted action contains artifacts not in previous action:
        //       _objs/fx/unit_0.pic.ddi ... Outputs: are equal
        // Forcing one object flavour leaves one action. PIC (rather than
        // --features=-supports_pic) is the one that matches the other engines:
        // it yields a PIE executable, which is what gcc/clang produce by default
        // for everyone else in the table.
        argv.push_back("--force_pic");
        if (job.variant != Variant::Headers) {
            // Both are required and they fail differently: without the first,
            // `attribute module_interfaces: requires --experimental_cpp_modules`;
            // without the second, `the feature cpp_modules must be enabled`.
            argv.push_back("--experimental_cpp_modules");
            argv.push_back("--features=cpp_modules");
        }
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
            return platform::run(argv, job.project_dir, job.log_path, job.timeout_s);
        }
        return platform::run(argv, job.project_dir, job.log_path, job.timeout_s);
    }

    void clean(const Job& job) const override {
        // Deliberately NOT --expunge: that would drop the downloaded toolchain
        // and turn a build measurement into a provisioning measurement.
        platform::run({"bazel", "clean"}, job.project_dir, job.log_path, job.timeout_s);
        platform::remove_tree(job.build_dir);
    }
};

export std::unique_ptr<Engine> make_bazel() { return std::make_unique<BazelEngine>(); }

}  // namespace bench::engines
