// merge_xpkg_versions_desc — the union view over a descriptor's per-OS
// version tables (#487). Ordering must follow SemVer on the PARSED value while
// display keeps the ORIGINAL key byte-for-byte (#363's lesson: an arbitrary
// index key cannot be reproduced from its parsed form).

#include <gtest/gtest.h>

import std;
import mcpp.manifest;

namespace {

TEST(XpkgVersionsMerge, OrdersSemverDescendingAcrossPlatforms) {
    std::vector<std::vector<std::string>> perPlatform = {
        {"1.0.0"},
        {"0.9.0", "1.2.0"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_EQ(merged[0], "1.2.0");
    EXPECT_EQ(merged[1], "1.0.0");
    EXPECT_EQ(merged[2], "0.9.0");
}

TEST(XpkgVersionsMerge, DeduplicatesKeysSharedAcrossPlatforms) {
    std::vector<std::vector<std::string>> perPlatform = {
        {"1.0.0", "0.9.0"},
        {"1.0.0", "0.8.0"},
        {"1.0.0"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_EQ(merged[0], "1.0.0");
    EXPECT_EQ(merged[1], "0.9.0");
    EXPECT_EQ(merged[2], "0.8.0");
}

TEST(XpkgVersionsMerge, ReleaseOutranksPrereleaseOfSameNumbers) {
    std::vector<std::vector<std::string>> perPlatform = {
        {"1.0.0-rc.1", "1.0.0"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    ASSERT_EQ(merged.size(), 2u);
    EXPECT_EQ(merged[0], "1.0.0");
    EXPECT_EQ(merged[1], "1.0.0-rc.1");
}

TEST(XpkgVersionsMerge, PrereleaseNumericIdentifiersCompareNumerically) {
    std::vector<std::vector<std::string>> perPlatform = {
        {"1.0.0-rc.2", "1.0.0-rc.10"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    ASSERT_EQ(merged.size(), 2u);
    EXPECT_EQ(merged[0], "1.0.0-rc.10");
    EXPECT_EQ(merged[1], "1.0.0-rc.2");
}

TEST(XpkgVersionsMerge, FourthSegmentComparesNumerically) {
    std::vector<std::vector<std::string>> perPlatform = {
        {"1.2.3.10", "1.2.3.9"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    ASSERT_EQ(merged.size(), 2u);
    EXPECT_EQ(merged[0], "1.2.3.10");
    EXPECT_EQ(merged[1], "1.2.3.9");
}

TEST(XpkgVersionsMerge, KeepsOriginalKeyForSuffixedBuilds) {
    // compat.glad publishes keys like "0.0.0-651a425": the suffix rides along
    // as pre-release identifiers for ordering, but what reaches the screen is
    // the key as written.
    std::vector<std::vector<std::string>> perPlatform = {
        {"0.0.0-651a425", "0.0.1"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    ASSERT_EQ(merged.size(), 2u);
    EXPECT_EQ(merged[0], "0.0.1");
    EXPECT_EQ(merged[1], "0.0.0-651a425");
}

TEST(XpkgVersionsMerge, UnparsableKeysSortLastByNameAndKeepTheirText) {
    std::vector<std::vector<std::string>> perPlatform = {
        {"unknown", "1.0.0", "beta-thing"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_EQ(merged[0], "1.0.0");
    EXPECT_EQ(merged[1], "beta-thing");
    EXPECT_EQ(merged[2], "unknown");
}

TEST(XpkgVersionsMerge, EmptyInputsYieldEmptyOutput) {
    EXPECT_TRUE(mcpp::manifest::merge_xpkg_versions_desc({}).empty());
    EXPECT_TRUE(
        mcpp::manifest::merge_xpkg_versions_desc({{}, {}}).empty());
}

// A truncated line must say it is truncated. Without the marker the default
// three-version view reads as the complete set, which is the one wrong
// conclusion the display can produce -- and both `mcpp search` and the
// `mcpp add` suggestion render through this function, so one assertion
// covers the contract for both.
TEST(JoinVersionsDesc, TruncationIsMarkedAndAnExactFitIsNot) {
    const std::vector<std::string> six = {"0.0.6", "0.0.5", "0.0.4",
                                          "0.0.3", "0.0.2", "0.0.1"};
    EXPECT_EQ(mcpp::manifest::join_versions_desc(six),
              "0.0.6, 0.0.5, 0.0.4, ...");
    EXPECT_EQ(mcpp::manifest::join_versions_desc(six, six.size()),
              "0.0.6, 0.0.5, 0.0.4, 0.0.3, 0.0.2, 0.0.1");

    // Exactly `keep` entries is not a truncation.
    const std::vector<std::string> three = {"2.0.0", "1.0.0", "0.9.0"};
    EXPECT_EQ(mcpp::manifest::join_versions_desc(three), "2.0.0, 1.0.0, 0.9.0");

    EXPECT_EQ(mcpp::manifest::join_versions_desc({}), "");
    EXPECT_EQ(mcpp::manifest::join_versions_desc({"1.0.0"}), "1.0.0");
}

} // namespace
