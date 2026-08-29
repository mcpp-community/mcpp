#include <gtest/gtest.h>

import std;
import mcpp.pm.dependency_selector;

// SUBSYSTEM-LEVEL. A dependency selector is part of what a manifest MEANS, so
// it lives here rather than in a package of its own -- and it is testable with
// nothing else present, which is the check that the layering is real.

TEST(SelectorGrammar, ADottedSelectorSplitsIntoItsSegments) {
    auto s = mcpp::pm::split_dependency_selector("mcpplibs.capi.lua");
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(s[0], "mcpplibs");
    EXPECT_EQ(s[1], "capi");
    EXPECT_EQ(s[2], "lua");
}

TEST(SelectorGrammar, ABareNameIsOneSegmentAndNotAnError) {
    // The unqualified spelling is the common case and resolves through the
    // namespace ladder later. Treating it as malformed here would move that
    // decision into the grammar, where the ladder is not visible.
    auto s = mcpp::pm::split_dependency_selector("cmdline");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0], "cmdline");
}

TEST(SelectorGrammar, JoiningIsTheInverseOfSplitting) {
    const std::string original = "mcpplibs.capi.lua";
    auto s = mcpp::pm::split_dependency_selector(original);
    EXPECT_EQ(mcpp::pm::join_dependency_segments(s, 0, s.size()), original);
}
