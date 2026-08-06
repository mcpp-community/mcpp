#include <gtest/gtest.h>

import std;
import mcpp.version_req;

using namespace mcpp::version_req;

TEST(VersionReq, ParseVersion) {
    auto v = parse_version("1.2.3");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->major(), 1); EXPECT_EQ(v->minor(), 2); EXPECT_EQ(v->patch(), 3);

    v = parse_version("0.5");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->major(), 0); EXPECT_EQ(v->minor(), 5); EXPECT_EQ(v->patch(), 0);

    v = parse_version("7");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->major(), 7); EXPECT_EQ(v->minor(), 0); EXPECT_EQ(v->patch(), 0);

    // #363: pre-release is PARSED, not stripped. Build metadata is dropped from
    // the order (SemVer §10) but the key's identity lives in the literal, which
    // pm/resolver.cppm carries separately.
    v = parse_version("1.2.3-beta.1+build42");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->str(), "1.2.3-beta.1");
    EXPECT_TRUE(v->isPrerelease());
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
    EXPECT_EQ(v->major(), 2026);
    EXPECT_EQ(v->minor(), 7);
    EXPECT_EQ(v->patch(), 27);
    EXPECT_EQ(v->revision(), 1);
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

// ─── #363: a version is an ORDER; the literal key is the identity ────────
//
// Every case below is a shape the real xim-pkgindex publishes today, not a
// hypothetical: `compat.imgui` ships 1.92.8 alongside 1.92.8-docking (a
// different upstream branch, a different tarball), `jdk-temurin` ships
// 25.0.4+7, `jdk-corretto` ships the five-segment 25.0.4.7.1, and `khistory`
// publishes only `pre-v0.0.5`.

TEST(VersionReq, PrereleaseSortsBelowItsRelease) {
    EXPECT_LT(*parse_version("1.92.8-docking"), *parse_version("1.92.8"));
    EXPECT_LT(*parse_version("1.3.3-beta.1"),   *parse_version("1.3.3"));
    // ...and above the previous release.
    EXPECT_LT(*parse_version("1.3.2"), *parse_version("1.3.3-beta.1"));
}

TEST(VersionReq, PrereleaseIdentifiersCompareBySemVerRules) {
    // Numeric identifiers compare numerically, not lexicographically.
    EXPECT_LT(*parse_version("1.0.0-rc.9"), *parse_version("1.0.0-rc.10"));
    // Numeric identifiers rank BELOW alphanumeric ones (SemVer §11.4.3).
    EXPECT_LT(*parse_version("1.0.0-1"), *parse_version("1.0.0-alpha"));
    // A longer identifier list outranks its own prefix (§11.4.4).
    EXPECT_LT(*parse_version("1.0.0-alpha"), *parse_version("1.0.0-alpha.1"));
    // Distinct pre-releases are never equal — the property that keeps
    // 1.92.8-docking and 1.92.8-nodocking addressable as two things.
    EXPECT_NE(*parse_version("1.92.8-docking"), *parse_version("1.92.8-legacy"));
}

TEST(VersionReq, RangesDoNotSeePrereleasesUnlessAskedFor) {
    // The #363 headline: ^1.92.8 must not silently pick up the docking branch.
    auto caret = parse_req("^1.92.8");   ASSERT_TRUE(caret);
    EXPECT_TRUE (matches(*caret, *parse_version("1.92.8")));
    EXPECT_FALSE(matches(*caret, *parse_version("1.92.8-docking")));

    // `*` never reaches a pre-release either.
    auto any = parse_req("*");           ASSERT_TRUE(any);
    EXPECT_FALSE(matches(*any, *parse_version("1.92.8-docking")));

    // Naming a pre-release at the same numeric tuple opts in.
    auto optIn = parse_req("^1.92.8-a"); ASSERT_TRUE(optIn);
    EXPECT_TRUE (matches(*optIn, *parse_version("1.92.8-docking")));

    // Exact still addresses it (this is the path `imgui = "1.92.8-docking"`
    // takes after try_merge_semver canonicalises the literal to "=...").
    auto exact = parse_req("=1.92.8-docking"); ASSERT_TRUE(exact);
    EXPECT_TRUE (matches(*exact, *parse_version("1.92.8-docking")));
    EXPECT_FALSE(matches(*exact, *parse_version("1.92.8")));
}

TEST(VersionReq, CaretUpperBoundDoesNotLeakIntoTheNextMajorsPrerelease) {
    // 2.0.0-alpha sorts BELOW 2.0.0, so a plain `v < upper` bound admitted it.
    auto r = parse_req("^1.2.3");   ASSERT_TRUE(r);
    EXPECT_FALSE(matches(*r, *parse_version("2.0.0-alpha")));
    EXPECT_FALSE(matches(*r, *parse_version("2.0.0")));
    EXPECT_TRUE (matches(*r, *parse_version("1.9.9")));
}

TEST(VersionReq, FiveSegmentKeysAreNotTruncated) {
    // jdk-corretto's scheme: <feature>.<interim>.<update>.<build>.<revision>.
    // Truncating at four made 25.0.4.7.1 and 25.0.4.7.2 compare EQUAL.
    EXPECT_LT(*parse_version("25.0.4.7.1"), *parse_version("25.0.4.7.2"));
    EXPECT_LT(*parse_version("21.0.12.8.1"), *parse_version("25.0.4.7.1"));
    EXPECT_EQ(parse_version("25.0.4.7.1")->str(), "25.0.4.7.1");
    // Insignificant trailing zeros still compare equal, at any length.
    EXPECT_EQ(*parse_version("1.2"), *parse_version("1.2.0.0.0"));
}

TEST(VersionReq, BuildMetadataIsExcludedFromPrecedenceButParses) {
    auto v = parse_version("25.0.4+7");
    ASSERT_TRUE(v);
    EXPECT_EQ(*v, *parse_version("25.0.4"));   // SemVer §10
    EXPECT_EQ(v->str(), "25.0.4");
}

TEST(VersionReq, UnorderableKeysAreRejectedRatherThanTruncated) {
    // llama.cpp-style build numbers, and the two non-numeric keys the real
    // index publishes. Each must be an ERROR here (→ exact-match-only in the
    // resolver), never a silent parse that collapses onto something else.
    EXPECT_FALSE(parse_version("b10069").has_value());
    EXPECT_FALSE(parse_version("latest").has_value());
    EXPECT_FALSE(parse_version("nightly").has_value());
    EXPECT_FALSE(parse_version("pre-v0.0.5").has_value());
    // Trailing garbage used to parse as 1.2.3 — and therefore compare EQUAL
    // to it, the same silent merge the extra segments exist to prevent.
    EXPECT_FALSE(parse_version("1.2.3abc").has_value());
    EXPECT_FALSE(parse_version("1.2.").has_value());
    EXPECT_FALSE(parse_version("1.2.3-").has_value());
    EXPECT_FALSE(parse_version("1.2.3+").has_value());
}

TEST(VersionReq, ChooseAllReportsPrecedenceTies) {
    // 25.0.4 and 25.0.4+7 are two addresses at one precedence. choose_all must
    // hand BOTH back so the resolver can refuse instead of guessing.
    std::vector<Version> avail = {
        *parse_version("25.0.4"),
        *parse_version("25.0.4+7"),
        *parse_version("21.0.12"),
    };
    auto r = parse_req("*");   ASSERT_TRUE(r);
    auto best = choose_all(*r, avail);
    ASSERT_EQ(best.size(), 2u);
    EXPECT_EQ(best[0], 0u);
    EXPECT_EQ(best[1], 1u);

    // The ordinary case stays a single answer.
    std::vector<Version> plain = { *parse_version("1.0.0"), *parse_version("1.2.0") };
    EXPECT_EQ(choose_all(*r, plain).size(), 1u);
}

TEST(VersionReq, McppOwnDateVersionsAreUnaffectedByPrereleaseSupport) {
    // E0006 (index floor) compares mcpp's own version through this parser.
    // Adding pre-release/metadata handling must not move any of it.
    EXPECT_LT(*parse_version("2026.8.3.3"), *parse_version("2026.8.6.3"));
    EXPECT_LT(*parse_version("2026.8.1"),   *parse_version("2026.8.1.1"));
    EXPECT_EQ(*parse_version("2026.8.1.0"), *parse_version("2026.8.1"));
    EXPECT_FALSE(parse_version("2026.8.6.3")->isPrerelease());
    EXPECT_EQ(parse_version("2026.8.6.3")->str(), "2026.8.6.3");
}
