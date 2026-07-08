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
