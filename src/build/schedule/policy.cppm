// mcpp.build.schedule.policy — which scheduling shape this build uses, and why.
//
// ONE TABLE, ONE DECISION. The shape of a module build is a function of the
// compiler family and of the host, and both halves of that answer used to be
// implicit: the BMI-equivalence restat was a POSIX shell fragment inside the
// generated ninja command (so Windows silently had none), and the job count was
// whatever ninja defaulted to. Deriving the same decision in two places is how
// the two halves drifted apart. This module is the only place it is derived.
//
// `decide()` IS PURE. No filesystem, no processes, no environment — a caller
// hands it facts and gets a Decision plus the sentence explaining it, so the
// table is unit-testable without a toolchain and the reason can be printed,
// logged and written into build.ninja unchanged. `requested_switch()` is the
// one impure function here, and it is impure on purpose: the switch has to be
// read somewhere, and two callers each doing env-then-manifest in their own
// order is exactly the duplicate derivation this module exists to prevent.
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
//     clang  TwoPhase       two ORDINARY edges over the same source: one emits
//                           only the BMI, one emits only the object. No process
//                           machinery, portable by construction.
//                           MEASURED (22.1.8, src/build/prepare.cppm):
//                           BMI edge 1.67 s vs 7.35 s for the single edge, and
//                           the object edge is byte-identical to the one the
//                           single edge produced.
//                           The object edge recompiles the SOURCE rather than
//                           reading the BMI back. `-c x.pcm` does work, but only
//                           against clang's *full* BMI, and publishing those to
//                           importers makes clang 22.1.8 miscompile a downstream
//                           TU (see BmiTraits::bmiOnlyFlags). Front-end work is
//                           therefore done twice — measured on the whole
//                           project it still wins at every job count tried:
//                           -j4 56.3 s → 37.6 s, -j8 34.0 s → 25.6 s,
//                           -j32 32.0 s → 18.0 s.
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
import mcpp.manifest;
import mcpp.platform.capacity;

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

// The one place the switch is READ. `decide` above stays pure — a caller hands
// it facts — but the switch itself has to come from somewhere, and having two
// callers each read env-then-manifest in their own order is precisely the
// duplicate-derivation this module exists to prevent.
//
// Precedence matches every other mcpp switch: environment beats manifest.
std::string requested_switch(const manifest::Manifest& m);

// How many compilers this machine should run at once.
//
// Precedence: MCPP_JOBS (where `--jobs` lands) > `[build] jobs` > 0, meaning
// "say nothing" and leave the backend's own default. The default is unchanged
// on purpose: altering everyone's concurrency is a behaviour change.
//
// `auto` is resolved HERE, against the machine doing the build, never frozen
// into a manifest. Measured on this repository: the cold self-build takes 81.0s
// at -j8 and 79.9s at -j32 — 4x the workers for 1.4%, because the build is
// latency-bound — while a single module compile peaks at 0.5–1.0 GB, so the
// extra jobs are pure memory pressure. On a high-core, modest-RAM machine the
// backend default swaps.
//
// `onInvalid` is called with the offending text instead of warning directly, so
// this stays free of any UI dependency and remains testable.
int resolve_jobs(const manifest::Manifest& m,
                 const std::function<void(std::string_view)>& onInvalid = {});

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
    // `auto` is OFF until the split graph has been through CI on every
    // platform. A scheduling change that is wrong is wrong SILENTLY — a missed
    // header dependency does not fail, it just stops rebuilding — so this does
    // not become the default on the strength of one machine. `on` selects it.
    if (requested != "on") {
        d.reason = "auto: the split schedule is opt-in until it has been "
                   "verified on every platform (set bmi_schedule = \"on\")";
        d.ninjaJobs = cap;
        return d;
    }

    switch (tc.compiler) {
        case toolchain::CompilerId::Clang:
            d.strategy = Strategy::TwoPhase;
            d.reason = "clang: a BMI-only invocation costs ~23% of a full "
                       "compile, so importers wait on that instead";
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

int resolve_jobs(const manifest::Manifest& m,
                 const std::function<void(std::string_view)>& onInvalid) {
    auto from_text = [&](std::string_view v) -> std::optional<int> {
        if (v.empty()) return std::nullopt;
        if (v == "auto") {
            const auto cap = platform::capacity::host_capacity();
            return platform::capacity::recommended_jobs(cap);
        }
        int n = 0;
        const auto* first = v.data();
        const auto* last  = v.data() + v.size();
        if (auto [p, ec] = std::from_chars(first, last, n);
            ec == std::errc{} && p == last && n > 0)
            return n;
        // A malformed value must not silently become "use the default" — that
        // is how a typo turns into a build that is mysteriously slower.
        if (onInvalid) onInvalid(v);
        return std::nullopt;
    };
    if (const char* e = std::getenv("MCPP_JOBS"))
        if (auto n = from_text(e)) return *n;
    if (auto n = from_text(m.buildConfig.jobs)) return *n;
    return 0;
}

std::string requested_switch(const manifest::Manifest& m) {
    if (const char* e = std::getenv("MCPP_BMI_SCHEDULE"); e && *e) return std::string(e);
    if (!m.buildConfig.bmiSchedule.empty()) return m.buildConfig.bmiSchedule;
    return "auto";
}

}  // namespace mcpp::build::schedule
