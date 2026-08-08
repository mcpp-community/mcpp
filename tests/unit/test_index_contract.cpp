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

// The Upgrade advice must tell the user which install method THEY have. A
// single hardcoded install.sh one-liner misleads distro (AUR) users into
// installing a second copy that does not update the running binary.
TEST(IndexContract, E0006MessageSwitchesUpgradeAdviceByLayout) {
    using mcpp::pm::floor_violation;
    using mcpp::pm::e0006_message;
    auto violation = floor_violation("2026.8.3.3", "2026.7.28.2");
    ASSERT_TRUE(violation.has_value());

    // Non-distro (install.sh / xlings) layout keeps the install.sh one-liner
    // and always appends the recommended-installer note.
    auto script = e0006_message(*violation, /*distroManaged=*/false);
    EXPECT_NE(script.find("install.sh"), std::string::npos);
    EXPECT_EQ(script.find("distro-managed"), std::string::npos);
    EXPECT_NE(script.find("xlings update mcpp"), std::string::npos);
    EXPECT_NE(script.find("E0006"), std::string::npos);

    // Distro (AUR) layout swaps in the package-manager advice.
    auto distro = e0006_message(*violation, /*distroManaged=*/true);
    EXPECT_NE(distro.find("distro-managed"), std::string::npos);
    EXPECT_EQ(distro.find("re-run the install.sh one-liner"), std::string::npos);
    EXPECT_NE(distro.find("xlings update mcpp"), std::string::npos);
    EXPECT_NE(distro.find("E0006"), std::string::npos);
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
