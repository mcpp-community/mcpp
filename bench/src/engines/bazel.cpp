// bench.engines.bazel — implementation.
//
// `module bench.engines.bazel;` with no `export`: an implementation unit. The engine class
// stays declared in the interface and its bodies live here, so changing HOW
// this engine drives its tool does not change the BMI, and nothing that
// imports it has to be recompiled.
module bench.engines.bazel;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;

namespace bench::engines {

std::string_view BazelEngine::name() const{ return "bazel"; }

Availability BazelEngine::probe() const{
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

bool BazelEngine::supports(Variant v, std::string_view compiler) const{
        if (v == Variant::Headers) return true;
        return is_clang(compiler);
    }

std::string BazelEngine::unsupported_reason(Variant v, std::string_view compiler) const{
        if (v == Variant::Headers || is_clang(compiler)) return {};
        return "bazel 9.2 builds C++20 modules with clang, but its ddi aggregator "
               "cannot parse GCC's P1689 output (aggregate-ddi: \"Invalid JSON string\"); "
               "re-run with --compiler <clang++> to measure this cell";
    }

platform::RunResult BazelEngine::configure(const Job&) const{
        return {0.0, 0};   // MODULE.bazel/BUILD are the configuration
    }

std::string BazelEngine::unbuildable_reason(const Job& job) const{
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

platform::RunResult BazelEngine::build(const Job& job) const{
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
        //
        // ⚠️ POSIX ONLY. Windows has no PIC — code there is relocatable by
        // construction — so its toolchain does not enable `supports_pic`, and
        // asking for it is not ignored, it is fatal at ANALYSIS:
        //
        //     Error in fail: PIC compilation is requested but the toolchain does
        //     not support it (feature named 'supports_pic' is not enabled)
        //     ERROR: Analysis of target '//:fx' failed; build aborted
        //
        // Every bazel cell of the windows/clang fixture died there, before a
        // single file was compiled. The duplicate-action problem this flag
        // solves is a POSIX one to begin with: it comes from bazel producing
        // both a pic and a non-pic flavour of each object, which Windows does
        // not do.
        if constexpr (platform::OS_NAME != "windows")
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

void BazelEngine::clean(const Job& job) const{
        // Deliberately NOT --expunge: that would drop the downloaded toolchain
        // and turn a build measurement into a provisioning measurement.
        platform::run({"bazel", "clean"}, job.project_dir, job.log_path, job.timeout_s);
        platform::remove_tree(job.build_dir);
    }

std::unique_ptr<Engine> make_bazel() { return std::make_unique<BazelEngine>(); }

}  // namespace bench::engines
