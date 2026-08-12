// The build-shape policy: one table, asserted from both sides.
//
// `decide()` is pure precisely so this file needs no toolchain, no filesystem
// and no compiler — the table can be wrong in a way that only shows up as a
// slower build, which is the kind of wrong that never gets noticed.

#include <gtest/gtest.h>

import std;
import mcpp.build.schedule.policy;
import mcpp.toolchain.model;

using mcpp::build::schedule::Strategy;
using mcpp::build::schedule::decide;
using mcpp::toolchain::CompilerId;
using mcpp::toolchain::Toolchain;

namespace {
Toolchain with(CompilerId id) {
    Toolchain tc;
    tc.compiler = id;
    return tc;
}
}  // namespace

// The two mechanisms are COMPLEMENTARY, not interchangeable, and getting them
// backwards is silent: clang writes its BMI to the final path with O_TRUNC, so
// detach-codegen would hand importers a half-written file; gcc has no cheap
// BMI-only mode, so two-phase would just compile everything twice.
TEST(SchedulePolicy, EachCompilerGetsItsOwnMechanism) {
    EXPECT_EQ(decide(with(CompilerId::Clang), "auto", 8).strategy, Strategy::TwoPhase);
    EXPECT_EQ(decide(with(CompilerId::GCC), "auto", 8).strategy, Strategy::DetachCodegen);
}

// Unmeasured means None. A guess here is not a slow build, it is a miscompile:
// a BMI read while it is still being written is not a diagnostic.
TEST(SchedulePolicy, UnmeasuredCompilersStayConservative) {
    EXPECT_EQ(decide(with(CompilerId::MSVC), "auto", 8).strategy, Strategy::None);
    EXPECT_EQ(decide(with(CompilerId::Unknown), "auto", 8).strategy, Strategy::None);
}

// Asserted from BOTH sides: that "off" disables, and that the same input with
// "auto" does NOT. Checking only the first would pass an implementation that
// never enables anything at all.
TEST(SchedulePolicy, OffDisablesAndAutoDoesNot) {
    EXPECT_EQ(decide(with(CompilerId::GCC), "off", 8).strategy, Strategy::None);
    EXPECT_NE(decide(with(CompilerId::GCC), "auto", 8).strategy, Strategy::None);
}

// HAZARD 2, encoded. Under detach-codegen a compiler stops holding a ninja slot
// the moment it publishes its BMI, so ninja's -j is no longer a bound on how
// many compilers run. With the two equal, ninja's slots fill with edges that
// are merely sleeping, the ready frontier starves, and the schedule degenerates
// to the baseline — which is exactly what the first prototype measured.
TEST(SchedulePolicy, DetachCodegenGivesNinjaMoreSlotsThanCompilers) {
    const auto d = decide(with(CompilerId::GCC), "auto", 32);
    EXPECT_EQ(d.compilerCap, 32);
    EXPECT_GT(d.ninjaJobs, d.compilerCap);
}

// Two-phase runs ordinary compilers that hold their slot for the whole compile,
// so inflating -j there would only oversubscribe the machine.
TEST(SchedulePolicy, TwoPhaseLeavesTheJobCountAlone) {
    const auto d = decide(with(CompilerId::Clang), "auto", 32);
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
    const auto d = decide(with(CompilerId::GCC), "auto", 0);
    EXPECT_EQ(d.compilerCap, 0);
    EXPECT_EQ(d.ninjaJobs, 0);
}
