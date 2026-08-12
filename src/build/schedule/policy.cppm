// mcpp.build.schedule.policy — which scheduling shape this build uses, and why.
//
// ONE TABLE, ONE DECISION. The shape of a module build is a function of the
// compiler family and of the host, and both halves of that answer used to be
// implicit: the BMI-equivalence restat was a POSIX shell fragment inside the
// generated ninja command (so Windows silently had none), and the job count was
// whatever ninja defaulted to. Deriving the same decision in two places is how
// the two halves drifted apart. This module is the only place it is derived.
//
// PURE. No filesystem, no processes, no environment reads — a caller passes the
// facts in and gets a Decision plus the sentence explaining it. That is what
// makes the policy unit-testable without a toolchain, and what lets the reason
// be printed, logged and written into build.ninja unchanged.
//
// THE MEASUREMENTS BEHIND THE TABLE (2026-08-13, mcpp building itself: 138
// module interface units, 57k lines, i9-13900K, gcc@16.1.0 / llvm@22.1.8):
//
//   The build is 100% critical path. makespan 79.79 s, critical path 79.73 s,
//   average parallelism 3.94x of 32 hardware threads. `-j8` → `-j32` buys 1.4%;
//   cmake and xmake build the same sources in 94.5 s and 94.6 s. Nothing outside
//   the graph's shape moves it.
//
//   86% of a module interface compile is code generation that no importer reads
//   (`-ftime-report`: opt-and-generate 14.08 s of 16.2 s). So the lever is:
//   unblock importers when the BMI is ready, not when the compiler exits.
//
//   HOW that is done differs per compiler, and the two mechanisms are
//   COMPLEMENTARY — each family supports exactly one:
//
//     clang  TwoPhase       `--precompile` emits the BMI, `-c x.pcm` emits the
//                           object: two ordinary edges, no process machinery,
//                           portable by construction. BMI ready at 57% of a
//                           single-phase compile for +9.6% total CPU.
//                           clang CANNOT use DetachCodegen — strace shows it
//                           writes the BMI to the final path with O_TRUNC, so a
//                           reader can observe a half-written file.
//
//     gcc    DetachCodegen  no cheap BMI-only mode exists (`-fmodule-only` costs
//                           99% of a full compile: it skips writing the object,
//                           not the back end), but gcc publishes the BMI with
//                           rename(), so the final path appearing is a sound
//                           signal. BMI ready at ~22%; cold build 80.5 s → 39.2 s.
//
//     msvc   None           unmeasured. `/ifcOnly`'s cost and whether `.ifc` is
//                           published atomically are both unknown, and guessing
//                           either wrong fails silently — a half-read BMI is not
//                           a diagnostic, it is a miscompile.
export module mcpp.build.schedule.policy;

import std;
import mcpp.toolchain.model;

export namespace mcpp::build::schedule {

enum class Strategy {
    None,           // one edge per module, importers wait for the compiler to exit
    TwoPhase,       // BMI edge + object edge, both ordinary compiler invocations
    DetachCodegen,  // BMI edge exits at publication; code generation continues
};

constexpr std::string_view to_string(Strategy s) {
    switch (s) {
        case Strategy::TwoPhase:      return "two-phase";
        case Strategy::DetachCodegen: return "detach-codegen";
        case Strategy::None:          break;
    }
    return "none";
}

struct Decision {
    Strategy strategy = Strategy::None;
    // ALWAYS populated, including for `None`. A scheduler that silently declines
    // to optimise is one nobody can debug: the question "why is my build not
    // using the fast shape?" has to have an answer that ships with the build.
    std::string reason;
    // Real concurrency bound. Under DetachCodegen a compiler stops holding a
    // ninja slot the moment it publishes, so ninja's -j is no longer a bound on
    // how many compilers run — this is (hazard 2 in detach_codegen).
    int compilerCap = 0;
    // What to hand ninja. MUST exceed compilerCap under DetachCodegen: with the
    // two equal, ninja's slots fill with edges that are merely sleeping, the
    // ready frontier starves, and the schedule degenerates to the baseline —
    // measured, and it is what made the first prototype read as a no-op.
    int ninjaJobs = 0;
};

// `requested` is the user's switch: "auto" (default), "on", "off". `hostJobs` is
// the already-resolved parallelism (`--jobs`, `[build] jobs`, or the backend
// default), i.e. how many compilers this machine should run at once.
Decision decide(const toolchain::Toolchain& tc, std::string_view requested, int hostJobs);

// ---------------------------------------------------------------------------

Decision decide(const toolchain::Toolchain& tc, std::string_view requested, int hostJobs) {
    Decision d;
    const int cap = hostJobs > 0 ? hostJobs : 0;

    if (requested == "off") {
        d.reason = "disabled by request";
        d.ninjaJobs = cap;
        return d;
    }

    switch (tc.compiler) {
        case toolchain::CompilerId::Clang:
            d.strategy = Strategy::TwoPhase;
            d.reason = "clang: --precompile publishes the BMI at ~57% of a "
                       "single-phase compile (+9.6% total CPU)";
            d.compilerCap = cap;
            // Two ordinary edges: a compiler always holds a ninja slot, so the
            // ordinary job count is still the real bound.
            d.ninjaJobs = cap;
            return d;

        case toolchain::CompilerId::GCC:
            d.strategy = Strategy::DetachCodegen;
            d.reason = "gcc: publishes the BMI with rename() at ~22% of the "
                       "compile, so importers can start before code generation";
            d.compilerCap = cap;
            // HAZARD 2. 6x is empirical: the prototype starved at 1x and was
            // saturated well before 6x (measured -j192 against a cap of 32).
            d.ninjaJobs = cap > 0 ? cap * 6 : 0;
            return d;

        case toolchain::CompilerId::MSVC:
            d.reason = "msvc: neither /ifcOnly's cost nor the atomicity of .ifc "
                       "publication has been measured; guessing either wrong is "
                       "silent, so the shape stays conservative";
            d.ninjaJobs = cap;
            return d;

        case toolchain::CompilerId::Unknown:
            break;
    }
    d.reason = "unknown compiler family";
    d.ninjaJobs = cap;
    return d;
}

}  // namespace mcpp::build::schedule
