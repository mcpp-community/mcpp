// bench.engines.cmake — implementation.
//
// `module bench.engines.cmake;` with no `export`: an implementation unit. The engine class
// stays declared in the interface and its bodies live here, so changing HOW
// this engine drives its tool does not change the BMI, and nothing that
// imports it has to be recompiled.
module bench.engines.cmake;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;

namespace bench::engines {

std::string_view CMakeEngine::name() const{ return "cmake"; }

Availability CMakeEngine::probe() const{
        auto a = probe_program("cmake", {"cmake", "--version"});
        if (!a.present) return a;
        // Ninja is not optional here: the Makefile generator cannot express
        // dyndep, so modules simply do not build. Reporting the real reason
        // beats a confusing configure failure later.
        if (!platform::have_program({"ninja", "--version"}))
            return {false, "cmake present but ninja is not; the Makefile generator cannot build C++20 modules"};
        return {true, std::format("{} + ninja", a.note)};
    }

bool CMakeEngine::supports(Variant v, std::string_view) const{
        if (v == Variant::Headers) return true;
        const auto ver = version();
        return !(ver.major && ver.major < 4);
    }

std::string CMakeEngine::unsupported_reason(Variant v, std::string_view) const{
        if (v == Variant::Headers) return {};
        const auto ver = version();
        return std::format(
            "cmake {}.{} is too old for `import std;` — the experimental gate key "
            "changes with the version and these descriptions carry the 4.0 one "
            "(bench/matrix.json pins 4.0.2)", ver.major, ver.minor);
    }

platform::RunResult CMakeEngine::configure(const Job& job) const{
        std::vector<std::string> argv{
            "cmake", "-S", job.buildfile_dir.string(), "-B", job.build_dir.string(),
            "-G", "Ninja",
            std::format("-DCMAKE_BUILD_TYPE={}", job.profile == "debug" ? "Debug" : "Release"),
        };
        if (const auto cxx = resolve_cxx(job.compiler); !cxx.empty())
            argv.push_back(std::format("-DCMAKE_CXX_COMPILER={}", cxx));
        auto r = platform::run(argv, {}, job.log_path, job.timeout_s);

        // ── When configure fails, append CMake's OWN detection log ──────────
        //
        // CMake's user-facing errors about the standard library are summaries:
        //
        //     The "CXX_MODULE_STD" property ... requires that the
        //     "__CMAKE::CXX23" target exist, but it was not provided by the
        //     toolchain.  Reason: Only `libstdc++` is supported
        //
        // names neither what it looked for nor what it found. Everything
        // checkable from OUTSIDE has been checked for the linux/gcc arm and it
        // all agrees with a machine where the same cmake and the same gcc
        // succeed: the manifest is present on the runner, both sources it names
        // are present, and the runner's exact flag shape reproduces and works
        // locally. What is left is inside CMake's own probe, and CMake writes
        // that down — in CMakeConfigureLog.yaml, which nobody ever reads
        // because it lives in a build directory that CI deletes with the job.
        //
        // Appended to the cell's log ONLY on failure, so a green run costs
        // nothing and a red one carries its own evidence.
        if (!r.ok()) {
            const auto detail = job.build_dir / "CMakeFiles" / "CMakeConfigureLog.yaml";
            // GREP, then tail. The first version took the last 120 lines and got
            // the ABI/linker-id probe, because that is what CMake happens to
            // write last — the std-module detection this was added for sits
            // EARLIER in the file and was cut off. Same lesson as the build
            // logs: a tail answers "what happened at the end", not "why did it
            // fail".
            std::ofstream log(job.log_path, std::ios::app);
            if (log) {
                if (const auto hits = platform::log_grep(
                        detail,
                        {"CXX_MODULE_STD", "IMPORT_STD", "import std", "CXX23",
                         "modules.json", "libstdc++", "std module"},
                        40);
                    !hits.empty())
                    log << "\n--- CMakeConfigureLog.yaml (std-module entries) ---\n"
                        << hits;
                if (const auto tail = platform::tail_of(detail, 60); !tail.empty())
                    log << "\n--- CMakeConfigureLog.yaml (last 60 lines) ---\n"
                        << tail;
            }
        }
        return r;
    }

platform::RunResult CMakeEngine::build(const Job& job) const{
        std::vector<std::string> argv{"cmake", "--build", job.build_dir.string()};
        if (job.jobs > 0) { argv.push_back("-j"); argv.push_back(std::to_string(job.jobs)); }
        return platform::run(argv, {}, job.log_path, job.timeout_s);
    }

CMakeEngine::Version CMakeEngine::version() const{
        if (!version_) {
            Version v;
            const auto a = probe_program("cmake", {"cmake", "--version"});
            if (a.present) {
                // "cmake version X.Y.Z" — scan to the first digit rather than
                // splitting on spaces, since the banner is localised on some
                // builds and a missing version must read as 0, not as "new".
                const auto pos = a.note.find_first_of("0123456789");
                if (pos != std::string::npos)
                    std::sscanf(a.note.c_str() + pos, "%d.%d", &v.major, &v.minor);
            }
            version_ = v;
        }
        return *version_;
    }

void CMakeEngine::clean(const Job& job) const{ platform::remove_tree(job.build_dir); }

std::unique_ptr<Engine> make_cmake() { return std::make_unique<CMakeEngine>(); }

}  // namespace bench::engines
