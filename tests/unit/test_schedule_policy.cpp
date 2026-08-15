// The build-shape policy: one table, asserted from both sides.
//
// `decide()` is pure precisely so this file needs no toolchain, no filesystem
// and no compiler — the table can be wrong in a way that only shows up as a
// slower build, which is the kind of wrong that never gets noticed.

#include <gtest/gtest.h>
#include <cstdlib>

import std;
import mcpp.build.schedule.policy;
import mcpp.toolchain.model;
import mcpp.manifest;

using mcpp::build::schedule::Strategy;
using mcpp::build::schedule::decide;
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
    ScopedVar(std::string name, const char* value) : name_(std::move(name)) {
        if (const char* old = std::getenv(name_.c_str()); old) { had_ = true; old_ = old; }
        apply(value);
    }
    ~ScopedVar() { apply(had_ ? old_.c_str() : nullptr); }
    ScopedVar(const ScopedVar&) = delete;
    ScopedVar& operator=(const ScopedVar&) = delete;
private:
    void apply(const char* v) {
#if defined(_WIN32)
        ::_putenv_s(name_.c_str(), v ? v : "");
#else
        if (v) ::setenv(name_.c_str(), v, 1); else ::unsetenv(name_.c_str());
#endif
    }
    std::string name_;
    bool had_ = false;
    std::string old_;
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
TEST(SchedulePolicy, EachCompilerGetsItsOwnMechanism) {
    EXPECT_EQ(decide(with(CompilerId::Clang), "on", 8).strategy, Strategy::TwoPhase);
    EXPECT_EQ(decide(with(CompilerId::GCC), "on", 8).strategy, Strategy::DetachCodegen);
}

// Unmeasured means None. A guess here is not a slow build, it is a miscompile:
// a BMI read while it is still being written is not a diagnostic.
// `auto` is OFF for now — pinned, because it is a decision rather than a gap.
TEST(SchedulePolicy, AutoIsOptInForNow) {
    EXPECT_EQ(decide(with(CompilerId::GCC), "auto", 8).strategy, Strategy::None);
    EXPECT_NE(decide(with(CompilerId::GCC), "on", 8).strategy, Strategy::None);
}

TEST(SchedulePolicy, UnmeasuredCompilersStayConservative) {
    EXPECT_EQ(decide(with(CompilerId::MSVC), "on", 8).strategy, Strategy::None);
    EXPECT_EQ(decide(with(CompilerId::Unknown), "on", 8).strategy, Strategy::None);
}

// Asserted from BOTH sides: that "off" disables, and that the same input with
// "auto" does NOT. Checking only the first would pass an implementation that
// never enables anything at all.
TEST(SchedulePolicy, OffDisablesAndAutoDoesNot) {
    EXPECT_EQ(decide(with(CompilerId::GCC), "off", 8).strategy, Strategy::None);
    EXPECT_NE(decide(with(CompilerId::GCC), "on", 8).strategy, Strategy::None);
}

// HAZARD 2, encoded. Under detach-codegen a compiler stops holding a ninja slot
// the moment it publishes its BMI, so ninja's -j is no longer a bound on how
// many compilers run. With the two equal, ninja's slots fill with edges that
// are merely sleeping, the ready frontier starves, and the schedule degenerates
// to the baseline — which is exactly what the first prototype measured.
TEST(SchedulePolicy, DetachCodegenGivesNinjaMoreSlotsThanCompilers) {
    const auto d = decide(with(CompilerId::GCC), "on", 32);
    EXPECT_EQ(d.compilerCap, 32);
    EXPECT_GT(d.ninjaJobs, d.compilerCap);
}

// Two-phase runs ordinary compilers that hold their slot for the whole compile,
// so inflating -j there would only oversubscribe the machine.
TEST(SchedulePolicy, TwoPhaseLeavesTheJobCountAlone) {
    const auto d = decide(with(CompilerId::Clang), "on", 32);
    EXPECT_EQ(d.ninjaJobs, d.compilerCap);
}

// A scheduler that silently declines to optimise cannot be debugged: "why is my
// build not using the fast shape?" has to have an answer that ships with the
// build. Every branch, including the ones that choose None.
TEST(SchedulePolicy, EveryDecisionCarriesAReason) {
    for (auto id : {CompilerId::GCC, CompilerId::Clang, CompilerId::MSVC,
                    CompilerId::Unknown}) {
        EXPECT_FALSE(decide(with(id), "auto", 8).reason.empty())
            << "no reason for compiler id " << static_cast<int>(id);
        EXPECT_FALSE(decide(with(id), "off", 8).reason.empty())
            << "no reason when disabled, compiler id " << static_cast<int>(id);
    }
}

// A host that reports nothing must not turn into "-j0" or a negative cap.
TEST(SchedulePolicy, ZeroJobsStaysZeroRatherThanBecomingNonsense) {
    const auto d = decide(with(CompilerId::GCC), "on", 0);
    EXPECT_EQ(d.compilerCap, 0);
    EXPECT_EQ(d.ninjaJobs, 0);
    // ...and it must SAY so. Under detach-codegen a cap of 0 disables the
    // semaphore, which is the only bound on how many compilers run at once, so
    // this state is not a neutral default — it is unbounded concurrency.
    EXPECT_NE(d.reason.find("bounded by nothing"), std::string::npos)
        << "a zero cap silently means no bound at all: " << d.reason;
}

// ⚠️ THE DEFAULT CONFIGURATION MUST STILL BE BOUNDED.
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
TEST(SchedulePolicy, DetachCodegenFallsBackToTheHostWhenNoJobCountWasGiven) {
    const auto d = decide(with(CompilerId::GCC), "on", /*hostJobs=*/0, /*autoJobs=*/24);
    EXPECT_EQ(d.strategy, Strategy::DetachCodegen);
    EXPECT_EQ(d.compilerCap, 24) << "the semaphore would be disabled";
    EXPECT_GT(d.ninjaJobs, d.compilerCap) << "hazard 2: -j must exceed the cap";

    // An explicit job count still wins over the fallback.
    const auto e = decide(with(CompilerId::GCC), "on", /*hostJobs=*/8, /*autoJobs=*/24);
    EXPECT_EQ(e.compilerCap, 8);

    // The fallback is only for the strategy that needs it; two-phase uses
    // ordinary edges, where ninja's -j is already the bound.
    const auto c = decide(with(CompilerId::Clang), "on", /*hostJobs=*/0, /*autoJobs=*/24);
    EXPECT_EQ(c.ninjaJobs, 0) << "clang must still defer to the backend default";
}

// `auto` is bounded by recommended_jobs' ceiling of 64, but `--jobs N` is only
// checked for `> 0`, so an absurd N reaches decide() intact and `cap * 6` was
// signed overflow — undefined behaviour, with a NEGATIVE `-j` handed to ninja as
// one of the friendlier outcomes. Asserted as "still positive and still greater
// than the cap" rather than against the clamp constant, so tuning the clamp does
// not require editing the test that exists to stop it going negative.
TEST(SchedulePolicy, AnAbsurdJobCountDoesNotOverflowIntoANegativeOne) {
    const auto d = decide(with(CompilerId::GCC), "on", 2000000000);
    EXPECT_GT(d.ninjaJobs, 0) << "ninja -j went non-positive";
    EXPECT_GT(d.ninjaJobs, 1) << "hazard 2: ninja must still outnumber the compilers";
}

// ─── requested_switch: a typo is a diagnostic, never a silent "auto" ───────
//
// This is the rule resolve_jobs already followed and this switch did not.
// `bmi_schedule = "ON"` was accepted, meant OFF, and explained itself with
// "the split schedule is opt-in until verified" — which reads as "you did not
// ask for it" to someone who just did.
TEST(SchedulePolicy, RequestedSwitchPassesTheThreeSpellingsThrough) {
    ScopedVar clear("MCPP_BMI_SCHEDULE", nullptr);
    EXPECT_EQ(requested_switch(with_schedule("on")), "on");
    EXPECT_EQ(requested_switch(with_schedule("off")), "off");
    EXPECT_EQ(requested_switch(with_schedule("auto")), "auto");
    EXPECT_EQ(requested_switch(with_schedule("")), "auto");   // unset
}

TEST(SchedulePolicy, RequestedSwitchReportsATypoInsteadOfSwallowingIt) {
    ScopedVar clear("MCPP_BMI_SCHEDULE", nullptr);
    for (const char* typo : {"ON", "On", "true", "yes", "1", "enabled"}) {
        std::string seen;
        const auto v = requested_switch(with_schedule(typo),
                                        [&](std::string_view bad) { seen = bad; });
        EXPECT_EQ(v, "auto") << typo << " must fall back to the default";
        EXPECT_EQ(seen, typo) << typo << " was accepted silently";
    }
}

// Both directions. Checking only that a typo warns would pass an implementation
// that warns about everything, including the spellings that are correct.
TEST(SchedulePolicy, RequestedSwitchStaysQuietForValidValues) {
    ScopedVar clear("MCPP_BMI_SCHEDULE", nullptr);
    for (const char* ok : {"on", "off", "auto"}) {
        bool warned = false;
        requested_switch(with_schedule(ok), [&](std::string_view) { warned = true; });
        EXPECT_FALSE(warned) << ok << " is valid but was reported as invalid";
    }
}

// The environment outranks the manifest — and is validated on the same terms.
// A typo'd MCPP_BMI_SCHEDULE must not fall through to the manifest either:
// silently honouring `[build] bmi_schedule = "on"` when the environment asked
// for something unparseable would make the warning a lie.
TEST(SchedulePolicy, EnvironmentBeatsManifestAndIsValidatedToo) {
    {
        ScopedVar on("MCPP_BMI_SCHEDULE", "off");
        EXPECT_EQ(requested_switch(with_schedule("on")), "off");
    }
    {
        ScopedVar bad("MCPP_BMI_SCHEDULE", "ON");
        std::string seen;
        EXPECT_EQ(requested_switch(with_schedule("on"),
                                   [&](std::string_view b) { seen = b; }), "auto");
        EXPECT_EQ(seen, "ON");
    }
}
