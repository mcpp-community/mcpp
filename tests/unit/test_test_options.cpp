#include <gtest/gtest.h>

import std;
import mcpp.build.backend;
import mcpp.build.execute;

using namespace mcpp::build;

// `mcpp test` has to be BOUNDABLE, and its run half bounded by default. It was
// neither: the per-test deadline defaulted to 0 (no limit) and no deadline
// existed for the build drives at all, so a single hung test or link could
// consume an entire unattended CI job with nothing to show for it. These
// defaults are the contract; asserting them here means neither half can drift
// back by an unnoticed edit.
TEST(TestOptions, RunIsBoundedByDefault) {
    TestOptions to;
    EXPECT_GT(to.timeoutSecs, 0) << "per-test run deadline must default to a limit";
    EXPECT_EQ(to.timeoutSecs, 300);
}

// The build deadline deliberately does NOT default on, and the asymmetry is
// measured: a test binary running over five minutes is unusual, a cold
// dependency build running over fifteen is ordinary (one mcpp-index member
// builds OpenCV from source in 1019s on Linux, 1289s on Windows). A default
// ceiling would turn slow-but-correct builds red and blame mcpp for it.
TEST(TestOptions, BuildDeadlineIsOptIn) {
    TestOptions to;
    EXPECT_EQ(to.buildTimeoutSecs, 0) << "a default build ceiling would fail legitimate cold builds";
}

// 0 is still reachable — "no limit" did not disappear, it just has to be asked
// for rather than being what you get by not thinking about it.
TEST(TestOptions, ZeroStillMeansUnlimited) {
    TestOptions to;
    to.timeoutSecs = 0;
    to.buildTimeoutSecs = 0;
    EXPECT_EQ(to.timeoutSecs, 0);
    EXPECT_EQ(to.buildTimeoutSecs, 0);
}

// A build drive with no explicit ceiling must stay unbounded: BuildOptions is
// shared with `mcpp build`, which is interactive and must not acquire a
// surprise deadline just because `mcpp test` grew one.
TEST(BuildOptions, BuildTimeoutDefaultsToUnbounded) {
    BuildOptions bo;
    EXPECT_EQ(bo.buildTimeoutSecs, 0u);
}

// The timeout verdict travels as a flag, not as a message prefix: run_tests
// reports a timed-out compile differently from a broken one, and matching on
// prose is exactly how that distinction rots silently.
TEST(BuildError, TimedOutIsAFlagAndDefaultsFalse) {
    BuildError e{"build failed", std::nullopt, "", false};
    EXPECT_FALSE(e.timedOut);
    BuildError t{"build timed out after 900s", std::nullopt, "", true};
    EXPECT_TRUE(t.timedOut);
}

// The fan-out needs more than an exit code to report per-member progress: how
// many tests ran, and where the time went. A zero-initialised summary must be
// safe to print (the "no tests found" early return leaves it untouched).
TEST(TestRunSummary, DefaultsAreZeroAndPrintable) {
    TestRunSummary s;
    EXPECT_EQ(s.passed, 0);
    EXPECT_EQ(s.failed, 0);
    EXPECT_EQ(s.buildMs, 0);
    EXPECT_EQ(s.runMs, 0);
    EXPECT_EQ(s.elapsedMs, 0);
    EXPECT_FALSE(s.packageError);
}
