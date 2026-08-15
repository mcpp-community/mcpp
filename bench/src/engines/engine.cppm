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
//   supports()   — not every engine can build every source form WITH EVERY
//                  COMPILER, and forcing a number out of one that cannot is
//                  worse than reporting that it cannot play. Measured examples:
//                  bazel 9.2 + rules_cc 0.2.22 builds C++20 modules with clang
//                  but not with gcc; meson 1.10.2 builds them with neither.
//                  Both are reported as `unavailable` WITH the measurement,
//                  never as a slow number.
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

    // Can it build this source form, WITH THIS COMPILER? The compiler is part
    // of the question: bazel builds C++20 modules with clang and fails with gcc
    // (its ddi aggregator cannot parse GCC's P1689 output), so "does bazel
    // support modules" has no answer that is independent of the run.
    // A `false` becomes `unavailable` with a reason, never a timing.
    virtual bool supports(Variant v, std::string_view compiler) const = 0;

    // Reason shown when supports() says no. Required, so the result file
    // explains itself without a reader consulting this source.
    virtual std::string unsupported_reason(Variant v, std::string_view compiler) const = 0;

    // "This engine cannot build THIS PROJECT" — the question `supports()` cannot
    // ask, because it only sees the variant and the compiler.
    //
    // The gap was not theoretical. bench/projects/mcpp/BUILD.bazel declares no
    // targets at all (bazel will not glob sources from outside its workspace, and
    // `import std;` has no bazel spelling), so `bazel build //...` succeeded
    // having built nothing, and the cell was published as
    //     bazel/clang/release/cold/mcpp-2026.8.11.3  0.43s
    // next to mcpp's 12s and cmake's 94s. Every layer behaved correctly on its
    // own: bazel exited 0, the runner timed it, the report printed it.
    //
    // Returning a non-empty reason marks the cell `unavailable` — a documented
    // gap — instead of `ok` with a number that is off by two orders of magnitude.
    // Empty (the default) means "nothing project-specific stops me".
    virtual std::string unbuildable_reason(const Job&) const { return {}; }

    // Does `compiler` resolve to a clang driver? Several engines' module
    // support is clang-only today.
    static bool is_clang(std::string_view compiler) {
        return compiler.find("clang") != std::string_view::npos;
    }

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
std::string resolve_cxx(std::string_view compiler);

// Trims to the first line and strips trailing whitespace and ANSI colour — some
// tools (xmake) colour their version banner, and a control sequence in a JSON
// result file is noise a reader has to decode.
std::string first_line(std::string_view text);

// Does this version banner actually say the program is missing?
//
// EVERY engine's probe needs this, not just the ones going through
// `probe_program`: mcpp and bazel have their own probes, and putting the test in
// only one of them left 36 cells per job still reported as engine FAILURES on
// macOS. One spelling, three callers.
bool looks_uninstalled(std::string_view banner);

std::string uninstalled_reason(std::string_view program, std::string_view banner);

// Shared helper: probe by running `<program> --version` and keeping the reported
// VERSION as the note. Engines with a different version flag override probe().
//
// The version, not just the name: a result file whose note reads "cmake" cannot
// answer "which cmake produced this?", and the answer moves the numbers a lot —
// cmake 4.0's module cold build is a different measurement from 3.28's. This is
// the same reason the report records host facts.
Availability probe_program(std::string_view program,
                                  const std::vector<std::string>& version_argv);

}  // namespace bench::engines
