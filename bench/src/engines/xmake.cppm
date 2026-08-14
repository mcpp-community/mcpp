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
// Decided from the RESOLVED DRIVER PATH, not from the `payload:` request.
// main.cpp rewrites `--compiler payload:gcc` into an absolute path long before
// an engine sees it, so testing `starts_with("payload:")` here is always false —
// which is how this silently stopped passing `--toolchain` at all while looking
// correct. The test that survives that rewrite is where the binary lives.
inline std::string payload_toolchain(std::string_view compiler) {
    if (compiler.find("/registry/data/xpkgs/") == std::string_view::npos &&
        compiler.find("\\registry\\data\\xpkgs\\") == std::string_view::npos)
        return {};
    const bool clang = compiler.find("clang") != std::string_view::npos;
    return clang ? "mcpp-clang" : "mcpp-gcc";
}

// The LLVM root a payload driver lives under: `…/xim-x-llvm/<ver>/bin/clang++`
// → `…/xim-x-llvm/<ver>`. Empty for anything that is not a payload clang.
inline std::string payload_sdk_root(std::string_view compiler) {
    if (compiler.find("/registry/data/xpkgs/") == std::string_view::npos &&
        compiler.find("\\registry\\data\\xpkgs\\") == std::string_view::npos)
        return {};
    if (compiler.find("clang") == std::string_view::npos) return {};
    const auto slash = compiler.find_last_of("/\\");
    if (slash == std::string_view::npos) return {};
    const auto bin = compiler.substr(0, slash);          // …/<ver>/bin
    const auto up  = bin.find_last_of("/\\");
    if (up == std::string_view::npos) return {};
    return std::string(bin.substr(0, up));               // …/<ver>
}

class XmakeEngine : public Engine {
public:
    std::string_view name() const override;

    Availability probe() const override;

    bool supports(Variant, std::string_view) const override;
    std::string unsupported_reason(Variant, std::string_view) const override;

    platform::RunResult configure(const Job& job) const override;

    platform::RunResult build(const Job& job) const override;

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
    void clean(const Job& job) const override;
};

export std::unique_ptr<Engine> make_xmake();

}  // namespace bench::engines
