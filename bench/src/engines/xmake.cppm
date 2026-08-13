// bench.engines.xmake — xmake, which drives its own scheduler rather than ninja.
export module bench.engines.xmake;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;
import bench.toolchain;

namespace bench::engines {

// The toolchain name ../common/xmake/payload.lua defines for a payload driver.
// Empty when the request is not a payload one (a bare `gcc`/`clang`/a path), in
// which case there is nothing pinned to name.
inline std::string payload_toolchain(std::string_view compiler) {
    if (!compiler.starts_with("payload:")) return {};
    return toolchain::resolves_to_clang(compiler) ? "mcpp-clang" : "mcpp-gcc";
}

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
        // ── How the payload driver is pinned, and why it is not always CXX ──
        //
        // A real project's description (bench/projects/*/xmake.lua) DEFINES the
        // payload as an xmake toolchain — compiler and its `-B<binutils>` /
        // `--sysroot` flags together — in ../common/xmake/payload.lua. Naming
        // that toolchain is the only way to get both halves.
        //
        // Setting CXX instead hands xrepo a compiler WITHOUT those flags, and
        // xmake then builds every dependency package with it. They fail:
        //     => install cmdline 0.0.2 .. failed
        //     => install mbedtls v3.6.7 .. failed
        //     => install ftxui v6.1.9 .. failed
        // — while the identical `xmake f --toolchain=mcpp-gcc` run by hand
        // succeeds, because there the toolchain carries the flags.
        //
        // The generated fixture has no such definition (bench.fixture.buildfiles
        // writes the payload flags inline), so it still takes CXX. The two cases
        // are told apart by whether the description lives beside the tree.
        // BOTH mechanisms, because they cover different builds:
        //
        //   --toolchain=mcpp-*   the PROJECT's targets. Carries the payload
        //                        compiler together with its -B/--sysroot.
        //   CC / CXX             the DEPENDENCY packages xrepo builds. xmake
        //                        does not apply the project toolchain to those,
        //                        so without this they compile with whatever
        //                        `cc` is first on PATH.
        //
        // In this repository that `cc` is the workspace xlings shim, whose
        // include path lacks the kernel UAPI headers its own glibc needs, so
        // every package build dies in a header three levels down:
        //     .../xim-x-glibc/2.39/include/bits/local_lim.h:38:10:
        //     fatal error: linux/limits.h: No such file or directory
        //       > in src/lua.c
        // The same command run by hand looked fine only because the packages
        // were already in xrepo's cache and never rebuilt.
        const bool own_description = !job.buildfile_dir.empty() &&
                                     job.buildfile_dir != job.project_dir;
        if (own_description) {
            if (const auto tc = payload_toolchain(job.compiler); !tc.empty()) {
                argv.push_back("--toolchain=" + tc);
                const auto cxx = resolve_cxx(job.compiler);
                if (!cxx.empty()) {
                    auto cc = cxx;
                    for (const auto& [from, to] : {std::pair{"clang++", "clang"},
                                                   std::pair{"g++", "gcc"}}) {
                        if (const auto at = cc.rfind(from); at != std::string::npos) {
                            cc.replace(at, std::string_view(from).size(), to);
                            break;
                        }
                    }
                    platform::ScopedEnv pin_cxx("CXX", cxx);
                    platform::ScopedEnv pin_cc("CC", cc);
                    return platform::run(argv, job.buildfile_dir, job.log_path, job.timeout_s);
                }
                return platform::run(argv, job.buildfile_dir, job.log_path, job.timeout_s);
            }
        }
        if (job.compiler == "clang") argv.push_back("--toolchain=llvm");
        // The driver is pinned through CXX so every engine compiles with the
        // SAME binary; without it xmake resolves whatever `g++` means on this
        // host, and the comparison silently becomes compiler-vs-compiler.
        if (const auto cxx = resolve_cxx(job.compiler); !cxx.empty()) {
            platform::ScopedEnv pin("CXX", cxx);
            return platform::run(argv, job.buildfile_dir, job.log_path, job.timeout_s);
        }
        return platform::run(argv, job.buildfile_dir, job.log_path, job.timeout_s);
    }

    platform::RunResult build(const Job& job) const override {
        std::vector<std::string> argv{"xmake", "build", "-P", job.buildfile_dir.string()};
        if (job.jobs > 0) argv.push_back(std::format("-j{}", job.jobs));
        return platform::run(argv, job.buildfile_dir, job.log_path, job.timeout_s);
    }

    // ⚠️ EVERY COMMAND RUNS FROM `buildfile_dir`, i.e. the `-P` directory, and
    // that is load-bearing rather than tidiness.
    //
    // xmake normalises `--builddir` (`-o`) to a path RELATIVE TO THE PROJECT
    // DIRECTORY, then resolves that relative path against the process's cwd when
    // it builds. Run it from anywhere other than `-P` and the two disagree. With
    // `-P bench/projects/mcpp` and `-o <workload>/build`, running from
    // `<workload>` put the artifacts in `<workload>/mcpp-2026.8.11.3/build` —
    // the workload path DOUBLED — while clean() went on removing
    // `<workload>/build`, which nothing ever wrote to.
    //
    // The symptom was a passing cell: `cold 0.60s` beside `touch-hub 82.79s`,
    // status `ok`, samples present. Every xmake real-project cold number the
    // suite produced was measuring an already-up-to-date tree. (The generated
    // fixture was unaffected — there `buildfile_dir == project_dir`, so the two
    // agreed by accident.) main.cpp now asserts cold > 2x that engine's own
    // noop, which is what turns this class of defect into a red run.
    //
    // `.xmake/` holds the resolved configuration — the counterpart of a cmake
    // cache or mcpp's resolution.json. Removing it would measure toolchain
    // detection rather than the build, so only the artifact dir goes.
    void clean(const Job& job) const override { platform::remove_tree(job.build_dir); }
};

export std::unique_ptr<Engine> make_xmake() { return std::make_unique<XmakeEngine>(); }

}  // namespace bench::engines
