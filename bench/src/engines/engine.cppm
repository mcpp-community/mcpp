// bench.engines.engine — the adapter contract every build engine implements.
//
// Adding an engine is: one new module implementing this interface, plus one line
// in bench.registry. The runner, the protocol, the scenarios and the CI matrix
// all stay untouched. That property is the whole reason this interface exists.
//
// Two of the methods look optional and are not:
//
//   probe()      — "not installed here" and "ran and failed" are OPPOSITE
//                  conclusions. Without probe, a missing bazel would be recorded
//                  as a slow or broken bazel. Protocol invariant 2.
//   supports()   — not every engine can build every source form. bazel's C++20
//                  module support is not comparable to CMake's, and forcing a
//                  number out of it would be worse than reporting that it cannot
//                  play. "不追求引擎功能对等" is a design decision, and this is
//                  where it is enforced.
export module bench.engines.engine;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;

export namespace bench::engines {

struct Availability {
    bool        present{};
    std::string note;     // version when present; why not when absent
};

class Engine {
public:
    virtual ~Engine() = default;

    virtual std::string_view name() const = 0;

    // Is this engine runnable on this machine right now?
    virtual Availability probe() const = 0;

    // Can it build this source form at all? A `false` becomes `unavailable`
    // with a reason, never a timing.
    virtual bool supports(Variant v) const = 0;

    // Reason shown when supports() says no. Required, so the result file
    // explains itself without a reader consulting this source.
    virtual std::string unsupported_reason(Variant v) const = 0;

    // One-time project setup (cmake/meson configure, xmake f, ...). Engines
    // with no configure step return success without doing anything.
    virtual platform::RunResult configure(const Job& job) const = 0;

    // The measured operation. Everything else exists to make this line fair.
    virtual platform::RunResult build(const Job& job) const = 0;

    // Remove build artifacts ONLY — never the toolchain or package caches.
    // A "cold build" is meant to measure building, not provisioning.
    virtual void clean(const Job& job) const = 0;
};

// Resolves `Job::compiler` to a concrete C++ driver.
//
// FAIRNESS: every engine that can be told which compiler to use MUST be told the
// same one, or the comparison measures compilers instead of build engines. The
// previous round of this benchmark pinned xmake to mcpp's hermetic g++ by hand
// for exactly this reason; here it is the harness's job.
//
// A value containing a separator is taken as a path and passed through, so a
// caller can pin a hermetic payload (`--compiler /path/to/g++`) rather than
// whatever `g++` happens to mean on this host — which, inside an xlings
// workspace, is a shim whose include search list moves with the workspace.
inline std::string resolve_cxx(std::string_view compiler) {
    if (compiler.empty() || compiler == "default") return {};
    if (compiler.find('/') != std::string_view::npos ||
        compiler.find('\\') != std::string_view::npos)
        return std::string(compiler);
    if (compiler == "gcc")   return "g++";
    if (compiler == "clang") return "clang++";
    return std::string(compiler);
}

// Shared helper: probe by running `<program> --version` and keeping the first
// line as the note. Engines with a different version flag override probe().
inline Availability probe_program(std::string_view program,
                                  const std::vector<std::string>& version_argv) {
    const auto r = platform::run(version_argv);
    if (r.exit_code < 0)
        return {false, std::format("{} not found on PATH", program)};
    if (r.exit_code != 0)
        return {false, std::format("{} present but `{}` exited {}", program,
                                   version_argv.size() > 1 ? version_argv[1] : "--version",
                                   r.exit_code)};
    return {true, std::string(program)};
}

}  // namespace bench::engines
