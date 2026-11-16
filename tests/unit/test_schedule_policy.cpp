// The build-shape policy: one table, asserted from both sides.
//
// `decide()` is pure precisely so this file needs no toolchain, no filesystem
// and no compiler — the table can be wrong in a way that only shows up as a
// slower build, which is the kind of wrong that never gets noticed.

#include <cstdlib>

import std;
import mcpplibs.mtest;
import mcpp.invocation;
import mcpp.build.schedule.policy;
import mcpp.toolchain.model;
import mcpp.manifest;

using namespace mcpplibs::mtest;

using mcpp::build::schedule::Strategy;
using mcpp::build::schedule::decide;
using mcpp::build::schedule::decide_shape;
using mcpp::build::schedule::requested_switch;
using mcpp::toolchain::CompilerId;
using mcpp::toolchain::Toolchain;

namespace {
Toolchain with(CompilerId id) {
    Toolchain tc;
    tc.compiler = id;
    return tc;
}

// MCPP_BMI_SCHEDULE outranks the manifest, so a stray one in the developer's
// shell would decide these tests instead of the code under test.
class ScopedVar {
public:
    ScopedVar(std::string name, const char* value)
        : inner_(std::move(name), value ? std::optional<std::string>(value) : std::nullopt) {}
private:
    mcpp::invocation::ScopedEnv inner_;   // stated for this invocation, as mcpp states its own
};

mcpp::manifest::Manifest with_schedule(std::string v) {
    mcpp::manifest::Manifest m;
    m.buildConfig.bmiSchedule = std::move(v);
    return m;
}
}  // namespace

// The two mechanisms are COMPLEMENTARY, not interchangeable, and getting them
// backwards is silent: clang writes its BMI to the final path with O_TRUNC, so
// detach-codegen would hand importers a half-written file; gcc has no cheap
// BMI-only mode, so two-phase would just compile everything twice.
UTest each_compiler_gets_its_own_mechanism{"SchedulePolicy/EachCompilerGetsItsOwnMechanism", [] {
    expect(eq(decide(with(CompilerId::Clang), "on", 8).strategy, Strategy::TwoPhase));
    expect(eq(decide(with(CompilerId::GCC), "on", 8).strategy, Strategy::DetachCodegen));
}};

// Unmeasured means None. A guess here is not a slow build, it is a miscompile:
// a BMI read while it is still being written is not a diagnostic.
// `auto` is OFF for now — pinned, because it is a decision rather than a gap.
UTest auto_is_opt_in_for_now{"SchedulePolicy/AutoIsOptInForNow", [] {
    expect(eq(decide(with(CompilerId::GCC), "auto", 8).strategy, Strategy::None));
    expect(ne(decide(with(CompilerId::GCC), "on", 8).strategy, Strategy::None));
}};

UTest unmeasured_compilers_stay_conservative{"SchedulePolicy/UnmeasuredCompilersStayConservative", [] {
    expect(eq(decide(with(CompilerId::MSVC), "on", 8).strategy, Strategy::None));
    expect(eq(decide(with(CompilerId::Unknown), "on", 8).strategy, Strategy::None));
}};

// Asserted from BOTH sides: that "off" disables, and that the same input with
// "auto" does NOT. Checking only the first would pass an implementation that
// never enables anything at all.
UTest off_disables_and_auto_does_not{"SchedulePolicy/OffDisablesAndAutoDoesNot", [] {
    expect(eq(decide(with(CompilerId::GCC), "off", 8).strategy, Strategy::None));
    expect(ne(decide(with(CompilerId::GCC), "on", 8).strategy, Strategy::None));
}};

// HAZARD 2, encoded. Under detach-codegen a compiler stops holding a ninja slot
// the moment it publishes its BMI, so ninja's -j is no longer a bound on how
// many compilers run. With the two equal, ninja's slots fill with edges that
// are merely sleeping, the ready frontier starves, and the schedule degenerates
// to the baseline — which is exactly what the first prototype measured.
UTest detach_codegen_gives_ninja_more_slots_than_compilers{"SchedulePolicy/DetachCodegenGivesNinjaMoreSlotsThanCompilers", [] {
    const auto d = decide(with(CompilerId::GCC), "on", 32);
    expect(eq(d.compilerCap, 32));
    expect(gt(d.ninjaJobs, d.compilerCap));
}};

// Two-phase runs ordinary compilers that hold their slot for the whole compile,
// so inflating -j there would only oversubscribe the machine.
UTest two_phase_leaves_the_job_count_alone{"SchedulePolicy/TwoPhaseLeavesTheJobCountAlone", [] {
    const auto d = decide(with(CompilerId::Clang), "on", 32);
    expect(eq(d.ninjaJobs, d.compilerCap));
}};

// A scheduler that silently declines to optimise cannot be debugged: "why is my
// build not using the fast shape?" has to have an answer that ships with the
// build. Every branch, including the ones that choose None.
UTest every_decision_carries_a_reason{"SchedulePolicy/EveryDecisionCarriesAReason", [] {
    for (auto id : {CompilerId::GCC, CompilerId::Clang, CompilerId::MSVC,
                    CompilerId::Unknown}) {
        expect(!(decide(with(id), "auto", 8).reason.empty()))
            << "no reason for compiler id " << static_cast<int>(id);
        expect(!(decide(with(id), "off", 8).reason.empty()))
            << "no reason when disabled, compiler id " << static_cast<int>(id);
    }
}};

// A host that reports nothing must not turn into "-j0" or a negative cap.
UTest zero_jobs_stays_zero_rather_than_becoming_nonsense{"SchedulePolicy/ZeroJobsStaysZeroRatherThanBecomingNonsense", [] {
    const auto d = decide(with(CompilerId::GCC), "on", 0);
    expect(eq(d.compilerCap, 0));
    expect(eq(d.ninjaJobs, 0));
    // ...and it must SAY so. Under detach-codegen a cap of 0 disables the
    // semaphore, which is the only bound on how many compilers run at once, so
    // this state is not a neutral default — it is unbounded concurrency.
    expect(ne(d.reason.find("bounded by nothing"), std::string::npos))
        << "a zero cap silently means no bound at all: " << d.reason;
}};

// THE DEFAULT CONFIGURATION MUST STILL BE BOUNDED.
//
// `resolve_jobs` returns 0 when the user passed neither `--jobs` nor
// `[build] jobs` — "say nothing, leave the backend's default". For every other
// strategy that is fine, because ninja's -j is a real bound. Under
// DetachCodegen it is NOT: a compiler stops holding its ninja slot the moment
// it publishes a BMI, so the semaphore is the only counter, and a cap of 0
// turns `acquire_token` into a no-op.
//
// This shipped: a plain `mcpp build` with `bmi_schedule = "on"` generated
// `sched_cap = 0`, i.e. ninja starting compiles as fast as BMIs appeared with
// nothing limiting them, on a workload whose single compile peaks near a
// gigabyte. The caller now resolves what the host would pick and hands it in.
UTest detach_codegen_falls_back_to_the_host_when_no_job_count_was_given{"SchedulePolicy/DetachCodegenFallsBackToTheHostWhenNoJobCountWasGiven", [] {
    const auto d = decide(with(CompilerId::GCC), "on", /*hostJobs=*/0, /*autoJobs=*/24);
    expect(eq(d.strategy, Strategy::DetachCodegen));
    expect(eq(d.compilerCap, 24)) << "the semaphore would be disabled";
    expect(gt(d.ninjaJobs, d.compilerCap)) << "hazard 2: -j must exceed the cap";

    // An explicit job count still wins over the fallback.
    const auto e = decide(with(CompilerId::GCC), "on", /*hostJobs=*/8, /*autoJobs=*/24);
    expect(eq(e.compilerCap, 8));

    // The fallback is only for the strategy that needs it; two-phase uses
    // ordinary edges, where ninja's -j is already the bound.
    const auto c = decide(with(CompilerId::Clang), "on", /*hostJobs=*/0, /*autoJobs=*/24);
    expect(eq(c.ninjaJobs, 0)) << "clang must still defer to the backend default";
}};

// `auto` is bounded by recommended_jobs' ceiling of 64, but `--jobs N` is only
// checked for `> 0`, so an absurd N reaches decide() intact and `cap * 6` was
// signed overflow — undefined behaviour, with a NEGATIVE `-j` handed to ninja as
// one of the friendlier outcomes. Asserted as "still positive and still greater
// than the cap" rather than against the clamp constant, so tuning the clamp does
// not require editing the test that exists to stop it going negative.
UTest an_absurd_job_count_does_not_overflow_into_a_negative_one{"SchedulePolicy/AnAbsurdJobCountDoesNotOverflowIntoANegativeOne", [] {
    const auto d = decide(with(CompilerId::GCC), "on", 2000000000);
    expect(gt(d.ninjaJobs, 0)) << "ninja -j went non-positive";
    expect(gt(d.ninjaJobs, 1)) << "hazard 2: ninja must still outnumber the compilers";
}};

// ─── resolve_jobs precedence (#564) ────────────────────────────────────────
//
// A KEY THE GENERATED FILE PROMISED AND NOTHING READ. mcpp writes
// `[build] default_jobs = 0` into `$MCPP_HOME/config.toml` itself; the loader
// parsed it into `GlobalConfig::defaultJobs`, and those were the only two
// mentions of the field in the repository. Setting it to 4 changed nothing:
// `ninja` ran with its own default, which is 10 on an 8-core machine, while a
// single module compile peaks at 0.5-1.0 GB.
//
// THE TEST ASSERTS THE ORDER, NOT THE WIRING. A fixture that sets only the
// global value and reads it back would pass just as well if the parameter had
// been placed ABOVE `MCPP_JOBS` instead of below it -- which would be the
// opposite of correct, because the global value is a property of the machine
// and the environment variable is this invocation. Each of the three levels is
// therefore checked against the level that must beat it.
UTest resolve_jobs_puts_the_machine_below_the_project_and_the_invocation{"SchedulePolicy/ResolveJobsPutsTheMachineBelowTheProjectAndTheInvocation", [] {
    mcpp::manifest::Manifest bare;                      // no [build] jobs
    auto with_jobs = [](std::string v) {
        mcpp::manifest::Manifest m;
        m.buildConfig.jobs = std::move(v);
        return m;
    };
    using mcpp::build::schedule::resolve_jobs;

    {   // The machine's value is used when nothing else says anything, and 0
        // still means "say nothing" for everyone who has not written the key.
        ScopedVar clear("MCPP_JOBS", nullptr);
        expect(eq(resolve_jobs(bare, {}, 4), 4));
        expect(eq(resolve_jobs(bare, {}, 0), 0));
        // A non-positive value reads as absent rather than as a bound: that is
        // what the generated template's `0` means, and a negative -j would be
        // handed straight to the backend.
        expect(eq(resolve_jobs(bare, {}, -1), 0));

        // The project beats the machine. A project that states a number has a
        // reason the machine cannot know.
        expect(eq(resolve_jobs(with_jobs("6"), {}, 4), 6));
    }
    {   // The invocation beats both. Without this leg the parameter could be
        // wired in above MCPP_JOBS and every other assertion here would pass.
        ScopedVar jobs("MCPP_JOBS", "2");
        expect(eq(resolve_jobs(bare, {}, 4), 2));
        expect(eq(resolve_jobs(with_jobs("6"), {}, 4), 2));
    }
}};

// ─── requested_switch: a typo is a diagnostic, never a silent "auto" ───────
//
// This is the rule resolve_jobs already followed and this switch did not.
// `bmi_schedule = "ON"` was accepted, meant OFF, and explained itself with
// "the split schedule is opt-in until verified" — which reads as "you did not
// ask for it" to someone who just did.
UTest requested_switch_passes_the_three_spellings_through{"SchedulePolicy/RequestedSwitchPassesTheThreeSpellingsThrough", [] {
    ScopedVar clear("MCPP_BMI_SCHEDULE", nullptr);
    expect(eq(requested_switch(with_schedule("on")), "on"));
    expect(eq(requested_switch(with_schedule("off")), "off"));
    expect(eq(requested_switch(with_schedule("auto")), "auto"));
    expect(eq(requested_switch(with_schedule("")), "auto"));   // unset
}};

UTest requested_switch_reports_a_typo_instead_of_swallowing_it{"SchedulePolicy/RequestedSwitchReportsATypoInsteadOfSwallowingIt", [] {
    ScopedVar clear("MCPP_BMI_SCHEDULE", nullptr);
    for (const char* typo : {"ON", "On", "true", "yes", "1", "enabled"}) {
        std::string seen;
        const auto v = requested_switch(with_schedule(typo),
                                        [&](std::string_view bad) { seen = bad; });
        expect(eq(v, "auto")) << typo << " must fall back to the default";
        expect(eq(seen, typo)) << typo << " was accepted silently";
    }
}};

// Both directions. Checking only that a typo warns would pass an implementation
// that warns about everything, including the spellings that are correct.
UTest requested_switch_stays_quiet_for_valid_values{"SchedulePolicy/RequestedSwitchStaysQuietForValidValues", [] {
    ScopedVar clear("MCPP_BMI_SCHEDULE", nullptr);
    for (const char* ok : {"on", "off", "auto"}) {
        bool warned = false;
        requested_switch(with_schedule(ok), [&](std::string_view) { warned = true; });
        expect(!warned) << ok << " is valid but was reported as invalid";
    }
}};

// The environment outranks the manifest — and is validated on the same terms.
// A typo'd MCPP_BMI_SCHEDULE must not fall through to the manifest either:
// silently honouring `[build] bmi_schedule = "on"` when the environment asked
// for something unparseable would make the warning a lie.
UTest environment_beats_manifest_and_is_validated_too{"SchedulePolicy/EnvironmentBeatsManifestAndIsValidatedToo", [] {
    {
        ScopedVar on("MCPP_BMI_SCHEDULE", "off");
        expect(eq(requested_switch(with_schedule("on")), "off"));
    }
    {
        ScopedVar bad("MCPP_BMI_SCHEDULE", "ON");
        std::string seen;
        expect(eq(requested_switch(with_schedule("on"),
                                   [&](std::string_view b) { seen = b; }), "auto"));
        expect(eq(seen, "ON"));
    }
}};

// EVERY STRATEGY CAPS ITS COMPILERS (2026.11.7.1): the capped executor is what
// registers a compiler in its build's generation, so a strategy without a split
// still has a cap -- the job count, else what this machine would choose.
UTest every_strategy_carries_a_compiler_cap{"SchedulePolicy/EveryStrategyCarriesACompilerCap", [] {
    for (auto id : {CompilerId::GCC, CompilerId::Clang, CompilerId::MSVC, CompilerId::Unknown}) {
        expect(eq(decide(with(id), "auto", 8).compilerCap, 8)) << static_cast<int>(id);
        expect(eq(decide(with(id), "off", 0, 6).compilerCap, 6)) << static_cast<int>(id);
    }
    // The shape alone is unchanged: only the split strategies carry a cap there.
    expect(eq(decide_shape(with(CompilerId::MSVC), "auto", 8).compilerCap, 0));
}};

