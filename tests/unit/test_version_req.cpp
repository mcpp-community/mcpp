#include <gtest/gtest.h>

import std;
import mcpp.version_req;

using namespace mcpp::version_req;

TEST(VersionReq, ParseVersion) {
    auto v = parse_version("1.2.3");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->major, 1); EXPECT_EQ(v->minor, 2); EXPECT_EQ(v->patch, 3);

    v = parse_version("0.5");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->major, 0); EXPECT_EQ(v->minor, 5); EXPECT_EQ(v->patch, 0);

    v = parse_version("7");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->major, 7); EXPECT_EQ(v->minor, 0); EXPECT_EQ(v->patch, 0);

    v = parse_version("1.2.3-beta.1+build42");
    ASSERT_TRUE(v) << "should strip pre-release";
    EXPECT_EQ(v->str(), "1.2.3");
}

TEST(VersionReq, ParseAny) {
    EXPECT_TRUE(parse_req("")->any);
    EXPECT_TRUE(parse_req("*")->any);
    EXPECT_TRUE(parse_req("  *  ")->any);
}

TEST(VersionReq, MatchExact) {
    auto r = parse_req("=1.2.3");  ASSERT_TRUE(r);
    EXPECT_TRUE (matches(*r, *parse_version("1.2.3")));
    EXPECT_FALSE(matches(*r, *parse_version("1.2.4")));
    EXPECT_FALSE(matches(*r, *parse_version("1.2.2")));
}

TEST(VersionReq, MatchCaretBare) {
    auto r = parse_req("1.2.3");   ASSERT_TRUE(r);
    EXPECT_TRUE (matches(*r, *parse_version("1.2.3")));
    EXPECT_TRUE (matches(*r, *parse_version("1.5.0")));
    EXPECT_TRUE (matches(*r, *parse_version("1.99.99")));
    EXPECT_FALSE(matches(*r, *parse_version("2.0.0")));
    EXPECT_FALSE(matches(*r, *parse_version("1.2.2")));
}

TEST(VersionReq, MatchCaretZeroMajor) {
    auto r = parse_req("0.5.3");   ASSERT_TRUE(r);
    EXPECT_TRUE (matches(*r, *parse_version("0.5.3")));
    EXPECT_TRUE (matches(*r, *parse_version("0.5.99")));
    EXPECT_FALSE(matches(*r, *parse_version("0.6.0"))) << "minor bump kills caret when major==0";
}

TEST(VersionReq, MatchTilde) {
    auto r = parse_req("~1.2.3");  ASSERT_TRUE(r);
    EXPECT_TRUE (matches(*r, *parse_version("1.2.3")));
    EXPECT_TRUE (matches(*r, *parse_version("1.2.99")));
    EXPECT_FALSE(matches(*r, *parse_version("1.3.0")));
    EXPECT_FALSE(matches(*r, *parse_version("1.2.2")));
}

TEST(VersionReq, MatchRangeAnd) {
    auto r = parse_req(">=1.2, <2.0");   ASSERT_TRUE(r);
    EXPECT_TRUE (matches(*r, *parse_version("1.2.0")));
    EXPECT_TRUE (matches(*r, *parse_version("1.99.99")));
    EXPECT_FALSE(matches(*r, *parse_version("1.1.99")));
    EXPECT_FALSE(matches(*r, *parse_version("2.0.0")));
}

TEST(VersionReq, ChooseBest) {
    std::vector<Version> avail = {
        *parse_version("1.0.0"),
        *parse_version("1.2.0"),
        *parse_version("1.2.5"),
        *parse_version("2.0.0"),
    };
    auto r = parse_req("^1.2");   ASSERT_TRUE(r);
    auto pick = choose(*r, avail);
    ASSERT_TRUE(pick);
    EXPECT_EQ(avail[*pick].str(), "1.2.5");
}

TEST(VersionReq, ChooseUnsatisfiable) {
    std::vector<Version> avail = { *parse_version("1.0.0"), *parse_version("2.0.0") };
    auto r = parse_req("^3");   ASSERT_TRUE(r);
    EXPECT_FALSE(choose(*r, avail).has_value());
}

TEST(VersionReq, RejectsGarbage) {
    EXPECT_FALSE(parse_req(">=foo").has_value());
    EXPECT_FALSE(parse_version("not-a-version").has_value());
}

// ─── Date-based versions (YYYY.M.D.N), mcpp's own scheme ────────────────
//
// Before the fourth segment existed, parse_version truncated at three and
// every release of a given day compared EQUAL — which silently disabled the
// E0006 index-floor check (index_contract.cppm compares mcpp's own version).

TEST(VersionReq, DateVersionParsesFourSegments) {
    auto v = parse_version("2026.7.27.1");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->major, 2026);
    EXPECT_EQ(v->minor, 7);
    EXPECT_EQ(v->patch, 27);
    EXPECT_EQ(v->revision, 1);
}

TEST(VersionReq, DateVersionOrdersWithinTheSameDay) {
    EXPECT_LT(*parse_version("2026.7.27.1"), *parse_version("2026.7.27.2"));
    // Numeric, not lexicographic: the 10th release of a day must outrank the 9th.
    EXPECT_LT(*parse_version("2026.7.27.9"), *parse_version("2026.7.27.10"));
    EXPECT_EQ(*parse_version("2026.7.27.3"), *parse_version("2026.7.27.3"));
}

TEST(VersionReq, DateVersionOutranksLegacySemver) {
    EXPECT_LT(*parse_version("0.0.109"), *parse_version("2026.7.27.1"));
}

TEST(VersionReq, ThreeSegmentVersionsStillCompareEqualAcrossWrittenLength) {
    // `components` must not leak into ordering.
    EXPECT_EQ(*parse_version("1.2"),   *parse_version("1.2.0"));
    EXPECT_EQ(*parse_version("1"),     *parse_version("1.0.0"));
    EXPECT_EQ(*parse_version("1.2.3"), *parse_version("1.2.3"));
}

// str() feeds pm/resolver.cppm's resolved-version return value, which becomes
// the lock entry and the xlings wire address — it has to reproduce the
// index's literal version key.
TEST(VersionReq, StrKeepsThreeSegmentNormalizationUnchanged) {
    EXPECT_EQ(parse_version("1.15.2")->str(), "1.15.2");
    EXPECT_EQ(parse_version("0.5")->str(),    "0.5.0");
    EXPECT_EQ(parse_version("7")->str(),      "7.0.0");
}

TEST(VersionReq, StrRendersTheFourthSegmentWhenWritten) {
    EXPECT_EQ(parse_version("2026.7.27.1")->str(), "2026.7.27.1");
    // A trailing ".0" is mcpp's formal-release marker and must NOT collapse
    // to three segments — "2026.8.1" is a key that does not exist.
    EXPECT_EQ(parse_version("2026.8.1.0")->str(), "2026.8.1.0");
}

TEST(VersionReq, CaretUpperBoundZeroesTheFourthSegment) {
    auto r = parse_req("^2026.7.27.3");   ASSERT_TRUE(r);
    EXPECT_TRUE (matches(*r, *parse_version("2026.7.27.3")));
    EXPECT_TRUE (matches(*r, *parse_version("2026.9.1.0")));
    // Upper bound is 2027.0.0.0 — it must not inherit the constraint's ".3".
    EXPECT_FALSE(matches(*r, *parse_version("2027.0.0.0")));
    EXPECT_FALSE(matches(*r, *parse_version("2027.0.0.2")));
}

TEST(VersionReq, TildeUpperBoundZeroesTheFourthSegment) {
    auto r = parse_req("~2026.7.27.3");   ASSERT_TRUE(r);
    EXPECT_TRUE (matches(*r, *parse_version("2026.7.99.0")));
    EXPECT_FALSE(matches(*r, *parse_version("2026.8.0.0")));
    EXPECT_FALSE(matches(*r, *parse_version("2026.8.0.5")));
}

TEST(VersionReq, ExactMatchDistinguishesSameDayReleases) {
    auto r = parse_req("=2026.7.27.1");   ASSERT_TRUE(r);
    EXPECT_TRUE (matches(*r, *parse_version("2026.7.27.1")));
    EXPECT_FALSE(matches(*r, *parse_version("2026.7.27.2")));
}

TEST(VersionReq, ChooseBestAmongDateVersions) {
    std::vector<Version> avail = {
        *parse_version("2026.7.27.1"),
        *parse_version("2026.7.27.10"),
        *parse_version("2026.7.27.9"),
    };
    auto r = parse_req("*");   ASSERT_TRUE(r);
    auto pick = choose(*r, avail);
    ASSERT_TRUE(pick);
    EXPECT_EQ(avail[*pick].str(), "2026.7.27.10");
}
