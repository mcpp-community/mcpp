#include <gtest/gtest.h>

import std;
import mcpp.pm.index_contract;

TEST(IndexContract, FloorViolationOrdering) {
    using mcpp::pm::floor_violation;
    EXPECT_FALSE(floor_violation("0.0.85", "0.0.85").has_value());
    EXPECT_FALSE(floor_violation("0.0.85", "0.0.90").has_value());
    EXPECT_FALSE(floor_violation("0.0.85", "1.0.0").has_value());
    auto v = floor_violation("0.0.85", "0.0.84");
    ASSERT_TRUE(v.has_value());
    EXPECT_NE(v->find("E0006"), std::string::npos);
    EXPECT_NE(v->find("0.0.85"), std::string::npos);
}

// The floor compares mcpp's OWN version, so the date scheme (YYYY.M.D.N) has
// to be ordered on all four segments. While parse_version truncated at three,
// every release of a given day compared equal and the floor let a too-old mcpp
// straight through — the exact failure this check exists to prevent.
TEST(IndexContract, FloorViolationSeparatesSameDayReleases) {
    using mcpp::pm::floor_violation;
    EXPECT_FALSE(floor_violation("2026.7.27.1", "2026.7.27.1").has_value());
    EXPECT_FALSE(floor_violation("2026.7.27.1", "2026.7.27.2").has_value());
    EXPECT_FALSE(floor_violation("2026.7.27.9", "2026.7.27.10").has_value());

    auto v = floor_violation("2026.7.27.5", "2026.7.27.1");
    ASSERT_TRUE(v.has_value()) << "a lower same-day ordinal must violate the floor";
    EXPECT_NE(v->find("E0006"), std::string::npos);
    EXPECT_NE(v->find("2026.7.27.5"), std::string::npos);
}

TEST(IndexContract, FloorViolationAcrossVersionSchemes) {
    using mcpp::pm::floor_violation;
    // A date-scheme mcpp satisfies any legacy 0.0.x floor.
    EXPECT_FALSE(floor_violation("0.0.109", "2026.7.27.1").has_value());
    // A legacy mcpp does not satisfy a date-scheme floor.
    EXPECT_TRUE(floor_violation("2026.7.27.1", "0.0.109").has_value());
}

TEST(IndexContract, EmptyOrMalformedNeverBricks) {
    using mcpp::pm::floor_violation;
    EXPECT_FALSE(floor_violation("", "0.0.84").has_value());
    EXPECT_FALSE(floor_violation("not-a-version", "0.0.84").has_value());
}

TEST(IndexContract, ReadContractRoundTrip) {
    auto dir = std::filesystem::temp_directory_path() / "mcpp_ic_test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream os(dir / "index.toml");
        os << "[index]\nspec = \"1\"\nmin_mcpp = \"0.0.85\"\nlatest_mcpp = \"0.0.86\"\n";
    }
    auto c = mcpp::pm::read_index_contract(dir);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->spec, "1");
    EXPECT_EQ(c->minMcpp, "0.0.85");
    EXPECT_EQ(c->latestMcpp, "0.0.86");
    std::filesystem::remove_all(dir);
    EXPECT_FALSE(mcpp::pm::read_index_contract(dir).has_value());
}
