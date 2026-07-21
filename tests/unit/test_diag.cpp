#include <gtest/gtest.h>

import std;
import mcpp.diag;

using namespace mcpp::diag;

namespace {

struct DiagTest : ::testing::Test {
    void SetUp()    override { reset(); }
    void TearDown() override { reset(); }
};

} // namespace

TEST_F(DiagTest, WarningAndDegradedAreCountedSeparately) {
    warning("manifest/schema", "unsupported key 'bogus'");
    degraded("build/depfile", "no GNU depfile on this toolchain",
             "edits to purview-included files will not trigger a rebuild");

    EXPECT_EQ(count(Severity::Warning), 1u);
    EXPECT_EQ(count(Severity::Degraded), 1u);
    EXPECT_EQ(records().size(), 2u);
}

TEST_F(DiagTest, IdenticalReportsAreDeduplicated) {
    for (int i = 0; i < 5; ++i)
        warning("modgraph/scan", "glob 'a/**' matched no source file");

    EXPECT_EQ(records().size(), 1u)
        << "a repeated report should render and record once";
}

TEST_F(DiagTest, SameDomainDifferentPayloadStaysDistinct) {
    // Two degradations in one domain that tell the user different things are
    // two records — dedup keys on the whole payload, not the domain.
    degraded("build/depfile", "no GNU depfile", "stale BMI possible");
    degraded("build/depfile", "no GNU depfile", "stale object possible");

    EXPECT_EQ(records().size(), 2u);
}

TEST_F(DiagTest, FormatKeepsWhatOnTheFirstLine) {
    // e2e assertions grep for the `what` substring; elaboration must not get
    // in front of it.
    warning("modgraph/scan", "[build].flags glob 'x/**' matched no source file");
    auto rs = records();
    ASSERT_EQ(rs.size(), 1u);
    EXPECT_EQ(rs[0].format(), "[build].flags glob 'x/**' matched no source file");

    reset();
    degraded("build/depfile", "what happened", "the impact", "the hint");
    rs = records();
    ASSERT_EQ(rs.size(), 1u);
    EXPECT_TRUE(rs[0].format().starts_with("what happened\n"));
    EXPECT_NE(rs[0].format().find("impact: the impact"), std::string::npos);
    EXPECT_NE(rs[0].format().find("hint: the hint"), std::string::npos);
}

TEST_F(DiagTest, StrictPromotesDegradationsOnly) {
    warning("manifest/schema", "just a warning");
    EXPECT_TRUE(flush(/*strict=*/true))
        << "a plain warning must not fail a --strict run";

    reset();
    degraded("build/depfile", "w", "i");
    EXPECT_FALSE(flush(/*strict=*/true));

    reset();
    degraded("build/depfile", "w", "i");
    EXPECT_TRUE(flush(/*strict=*/false));
}

TEST_F(DiagTest, FlushClearsRunState) {
    warning("a", "b");
    degraded("c", "d", "e");
    EXPECT_TRUE(flush(/*strict=*/false));
    EXPECT_EQ(records().size(), 0u);
    EXPECT_EQ(count(Severity::Degraded), 0u);
}

// Regression guard for the review finding on this batch: flush() is the ONLY
// place the --strict policy is settled, and it was initially never called
// outside this file — the channel reported degradations and then everyone
// ignored them, which is precisely the failure mode it exists to prevent.
// run_build_plan now calls it; this pins the contract flush() must honour.
TEST_F(DiagTest, FlushIsTheSolePolicyPointAndReportsFailureToTheCaller) {
    // No records at all: strict must not fail a clean build.
    EXPECT_TRUE(flush(/*strict=*/true));

    // A degradation under --strict must tell the caller to fail. The caller
    // (run_build_plan) turns this into a non-zero exit.
    degraded("build/depfile", "no depfile on this toolchain",
             "stale BMI possible after editing an included file");
    EXPECT_FALSE(flush(/*strict=*/true));

    // Same degradation without --strict: reported, not fatal.
    degraded("build/depfile", "no depfile on this toolchain",
             "stale BMI possible after editing an included file");
    EXPECT_TRUE(flush(/*strict=*/false));
}
