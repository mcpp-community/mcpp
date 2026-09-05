#include <gtest/gtest.h>

import std;
import mcpp.build.version_floor;

using mcpp::build::parse_version_floor;
using mcpp::build::parse_version_fact;
using mcpp::build::version_at_least;

// ── The two spellings ──────────────────────────────────────────────────────

TEST(VersionFloor, ReadsAFloor) {
    auto f = parse_version_floor("cuda.driver >= 12.0");
    ASSERT_TRUE(f.valid());
    EXPECT_EQ(f.name,    "cuda.driver");
    EXPECT_EQ(f.version, "12.0");
}

TEST(VersionFloor, ReadsAFact) {
    auto f = parse_version_fact("cuda.driver=12.4");
    ASSERT_TRUE(f.valid());
    EXPECT_EQ(f.name,    "cuda.driver");
    EXPECT_EQ(f.version, "12.4");
}

TEST(VersionFloor, AFloorIsNotReadAsAFact) {
    // Both spellings live in string lists, and `>=` contains `=`. Reading one
    // as the other would turn "needs at least 12.0" into "this machine has
    // 12.0" — a requirement silently becoming its own satisfaction.
    EXPECT_FALSE(parse_version_fact("cuda.driver >= 12.0").valid());
}

TEST(VersionFloor, AnythingElseIsNotAFloor) {
    EXPECT_FALSE(parse_version_floor("cuda.driver").valid());
    EXPECT_FALSE(parse_version_floor("cuda.driver >= ").valid());
    EXPECT_FALSE(parse_version_floor(" >= 12.0").valid());
    // Not a version: refused rather than read as `12`.
    EXPECT_FALSE(parse_version_floor("cuda.driver >= 12.").valid());
    EXPECT_FALSE(parse_version_floor("cuda.driver >= twelve").valid());
}

// ── Comparison ─────────────────────────────────────────────────────────────

TEST(VersionFloor, ComparesComponentwise) {
    EXPECT_EQ(version_at_least("12.4",  "12.0"), std::optional{true});
    EXPECT_EQ(version_at_least("12.0",  "12.4"), std::optional{false});
    EXPECT_EQ(version_at_least("13.0",  "12.9"), std::optional{true});
    // Not lexicographic: 12.10 is above 12.9.
    EXPECT_EQ(version_at_least("12.10", "12.9"), std::optional{true});
}

TEST(VersionFloor, AMissingComponentIsZero) {
    EXPECT_EQ(version_at_least("12",   "12.0"), std::optional{true});
    EXPECT_EQ(version_at_least("12.0", "12"),   std::optional{true});
    EXPECT_EQ(version_at_least("12",   "12.1"), std::optional{false});
}

TEST(VersionFloor, EitherSideUnreadableIsNoClaim) {
    // The whole reason this mechanism exists is that a wrong answer is worse
    // than no answer. A version that cannot be read yields neither.
    EXPECT_FALSE(version_at_least("unknown", "12.0").has_value());
    EXPECT_FALSE(version_at_least("12.0", "").has_value());
}

TEST(VersionFloor, NoVendorVocabularyIsRequired) {
    // The name is data. A backend this engine has never heard of compares the
    // same way, which is what keeps the comparison out of the vendors' reach.
    auto f = parse_version_floor("some.future.thing >= 4.2.1");
    ASSERT_TRUE(f.valid());
    EXPECT_EQ(f.name, "some.future.thing");
    EXPECT_EQ(version_at_least("4.3", f.version), std::optional{true});
    EXPECT_EQ(version_at_least("4.2", f.version), std::optional{false});
}
