// bench.engines.cmake — CMake + Ninja.
//
// CMake has supported C++20 named modules since 3.28 (with Ninja >= 1.11), so it
// is the reference point for "the mainstream way to build modules today".
module;
// std::sscanf for the version banner; <cstdio> is not reachable through
// `import std;` for the C-library names in the global namespace.
#include <cstdio>

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
        return {true, std::format("{} + ninja", a.note)};
    }

    // `import std;` sits behind an experimental gate whose KEY CHANGES WITH THE
    // CMAKE VERSION, and the descriptions in projects/ carry the CMake 4.0 one.
    // An older cmake does not reject the key, it simply does not recognise it —
    // so the gate stays shut and configure dies on the first `import std;` with
    // an error about the standard library rather than about the version.
    //
    // Reporting that as `failed` is what the runner images actually produced:
    // cmake 3.31.6 ships in every GitHub image, and EVERY module cell in the
    // matrix was recorded as a real finding against cmake. It is not one — it is
    // "this engine, at this version, cannot express this cell", which is exactly
    // what `unavailable` plus a reason is for.
    //
    // Only the module forms need it. `headers` builds fine on 3.28, which is why
    // those six cells were the only ones in the whole matrix that ever passed.
    bool supports(Variant v, std::string_view) const override {
        if (v == Variant::Headers) return true;
        const auto ver = version();
        return !(ver.major && ver.major < 4);
    }
    std::string unsupported_reason(Variant v, std::string_view) const override {
        if (v == Variant::Headers) return {};
        const auto ver = version();
        return std::format(
            "cmake {}.{} is too old for `import std;` — the experimental gate key "
            "changes with the version and these descriptions carry the 4.0 one "
            "(bench/matrix.json pins 4.0.2)", ver.major, ver.minor);
    }

    platform::RunResult configure(const Job& job) const override {
        std::vector<std::string> argv{
            "cmake", "-S", job.buildfile_dir.string(), "-B", job.build_dir.string(),
            "-G", "Ninja",
            std::format("-DCMAKE_BUILD_TYPE={}", job.profile == "debug" ? "Debug" : "Release"),
        };
        if (const auto cxx = resolve_cxx(job.compiler); !cxx.empty())
            argv.push_back(std::format("-DCMAKE_CXX_COMPILER={}", cxx));
        return platform::run(argv, {}, job.log_path, job.timeout_s);
    }

    platform::RunResult build(const Job& job) const override {
        std::vector<std::string> argv{"cmake", "--build", job.build_dir.string()};
        if (job.jobs > 0) { argv.push_back("-j"); argv.push_back(std::to_string(job.jobs)); }
        return platform::run(argv, {}, job.log_path, job.timeout_s);
    }

    // Parsed out of the probe banner ("cmake version 4.0.2"), and cached: the
    // support question is asked once per cell and spawning cmake each time would
    // add a process launch to every row of the matrix.
    struct Version { int major{}; int minor{}; };
    Version version() const {
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

    // Artifacts only — the configure result lives in the same directory, so a
    // "cold" build here re-runs configure. That is declared in the bench README
    // rather than papered over: cmake genuinely cannot separate the two without
    // keeping a second cache.
    void clean(const Job& job) const override { platform::remove_tree(job.build_dir); }

private:
    mutable std::optional<Version> version_;
};

export std::unique_ptr<Engine> make_cmake() { return std::make_unique<CMakeEngine>(); }

}  // namespace bench::engines
