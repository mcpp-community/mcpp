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
