// bench.engines.xmake — implementation.
//
// `module bench.engines.xmake;` with no `export`: an implementation unit. The engine class
// stays declared in the interface and its bodies live here, so changing HOW
// this engine drives its tool does not change the BMI, and nothing that
// imports it has to be recompiled.
module bench.engines.xmake;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.toolchain;
import bench.engines.engine;

namespace bench::engines {

std::string_view XmakeEngine::name() const { return "xmake"; }

Availability XmakeEngine::probe() const {
        return probe_program("xmake", {"xmake", "--version"});
    }

bool XmakeEngine::supports(Variant, std::string_view) const { return true; }

std::string XmakeEngine::unsupported_reason(Variant, std::string_view) const { return {}; }

platform::RunResult XmakeEngine::configure(const Job& job) const {
        std::vector<std::string> argv{
            "xmake", "f", "-y",
            // -P names the directory holding xmake.lua. For a fixture that is
            // the tree itself; for a real project the description lives beside
            // the bench and reaches back into the tree.
            "-P", job.buildfile_dir.string(),
            "-m", job.profile == "debug" ? "debug" : "release",
            "-o", job.build_dir.string(),
            // xmake's compiler cache is ON BY DEFAULT (`--ccache=y`) and it
            // lives OUTSIDE the build directory, so `clean()` cannot reach it.
            // A `cold` build then restores every object from it:
            //     seed build   105.059s
            //     timed run      0.106s   <- "cold"
            // which the report published as `cold 0.70s` against cmake's 103s.
            // Both invariants caught it (cold vs its own noop, and cold vs the
            // other engines on the same sources), which is the only reason this
            // is a comment rather than a number in the README.
            //
            // Disabled rather than declared as an asymmetry: neither mcpp nor
            // cmake has a compiler cache in this suite, so leaving it on would
            // not be "xmake is faster", it would be "xmake did not compile".
            // bazel's action cache is the one that IS declared instead — there
            // it cannot be turned off without also discarding the toolchain.
            "--ccache=n",
        };
        // ── Tell xmake where libc++ lives, or `import std;` has no provider ──
        //
        // The payload SHIPS the std module (share/libc++/v1/std.cppm), but xmake
        // looks for it under its own notion of an LLVM SDK and otherwise says
        //     warning: std and std.compat modules not found!
        //              maybe try to add --sdk=<PATH/TO/LLVM> or install libc++
        //     error: <mcpp> missing std dependency for module mcpp.build.provisions
        // — a message naming a module of the project under test, so it reads as
        // "this project is broken" rather than "the engine was not told where
        // its standard library is". Every scenario in the windows/clang cell
        // failed that way.
        //
        // Derived from the resolved driver (…/bin/clang++), which is the same
        // path the toolchain pin already produced, so there is nothing to keep
        // in step by hand.
        if (const auto sdk = payload_sdk_root(job.compiler); !sdk.empty())
            argv.push_back("--sdk=" + sdk);

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
        // ── clang: xmake's BUILT-IN llvm toolchain, plus a RUNTIME ──────────
        //
        // The custom `mcpp-clang` toolchain below carries the payload's `-B` and
        // include chain, which gcc needs. For clang it is actively wrong, and
        // the reason is not obvious enough to leave undocumented.
        //
        // xmake picks the std module by C++ LIBRARY, and reads the library from
        // the target's RUNTIME (rules/c++/modules/support.lua):
        //
        //     has_runtime("c++_shared","c++_static") -> "c++"     (libc++)
        //     ... no runtime given: fall back on the platform
        //     is_plat("linux", ...)                  -> "stdc++"  (libstdc++)
        //
        // So a clang build with no runtime declared is treated as libstdc++:
        // xmake looks for GCC's modules.json inside the LLVM payload, finds
        // nothing, and prints
        //
        //     warning: std and std.compat modules not found!
        //              maybe try to add --sdk=<PATH/TO/LLVM> or install libc++
        //
        // THAT SUGGESTION IS A DEAD END, and following it cost three rounds.
        // `--sdk` is only read on the `c++` branch, which was never reached. The
        // payload has carried `lib/<triple>/libc++.modules.json` — precisely
        // what that branch looks for — the entire time.
        //
        // Declaring the runtime on the TARGET does not fix it either: with the
        // custom standalone toolchain the branch still is not taken. What works,
        // verified end to end, is the built-in toolchain plus both flags —
        // measured: the warning disappears and xmake starts emitting module
        // BMIs.
        const bool payload_clang = !payload_sdk_root(job.compiler).empty();
        if (payload_clang) {
            argv.push_back("--toolchain=llvm");
            argv.push_back("--runtimes=c++_static");   // mcpp.toml: static_stdlib
            if (const auto cxx = resolve_cxx(job.compiler); !cxx.empty()) {
                auto cc = cxx;
                if (const auto at = cc.rfind("clang++"); at != std::string::npos)
                    cc.replace(at, 7, "clang");
                platform::ScopedEnv pin_cxx("CXX", cxx);
                platform::ScopedEnv pin_cc("CC", cc);
                return platform::run(argv, job.buildfile_dir, job.log_path, job.timeout_s);
            }
            return platform::run(argv, job.buildfile_dir, job.log_path, job.timeout_s);
        }

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
        // Decided from the RESOLVED DRIVER, not from the literal string "clang"
        // — main.cpp rewrites `--compiler payload:clang` into an absolute path
        // before any engine sees it, so `job.compiler == "clang"` is false in
        // exactly the cells that need this. xmake then fell back to g++ while
        // the description carried clang's flags:
        //     g++: error: unrecognized command-line option '--no-default-config'
        // Six fixture cells in the linux/clang job. Same rewrite, same mistake
        // as `payload_toolchain` above — which I fixed without checking whether
        // anything else tested the same string.
        if (job.compiler.find("clang") != std::string::npos)
            argv.push_back("--toolchain=llvm");
        // The driver is pinned through CXX so every engine compiles with the
        // SAME binary; without it xmake resolves whatever `g++` means on this
        // host, and the comparison silently becomes compiler-vs-compiler.
        if (const auto cxx = resolve_cxx(job.compiler); !cxx.empty()) {
            platform::ScopedEnv pin("CXX", cxx);
            return platform::run(argv, job.buildfile_dir, job.log_path, job.timeout_s);
        }
        return platform::run(argv, job.buildfile_dir, job.log_path, job.timeout_s);
    }

platform::RunResult XmakeEngine::build(const Job& job) const {
        std::vector<std::string> argv{"xmake", "build", "-P", job.buildfile_dir.string()};
        if (job.jobs > 0) argv.push_back(std::format("-j{}", job.jobs));
        return platform::run(argv, job.buildfile_dir, job.log_path, job.timeout_s);
    }

void XmakeEngine::clean(const Job& job) const { platform::remove_tree(job.build_dir); }

std::unique_ptr<Engine> make_xmake() { return std::make_unique<XmakeEngine>(); }

}  // namespace bench::engines
