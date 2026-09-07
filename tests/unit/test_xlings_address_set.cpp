#include <gtest/gtest.h>

import std;
import mcpp.xlings.address_set;

namespace addrset = mcpp::xlings::addrset;

namespace {

addrset::Claim project(std::string address) {
    return { std::move(address), "this project", 0 };
}
addrset::Claim from(std::string who, std::string address) {
    return { std::move(address), std::move(who), 1 };
}

std::vector<std::string> addresses(const addrset::Resolution& r) {
    std::vector<std::string> out;
    for (auto const& w : r.winners) out.push_back(w.address);
    return out;
}

} // namespace

// ─── Identity ──────────────────────────────────────────────────────────────

// The version is a constraint on a package, never part of its name. Two
// definitions of this used to coexist: the conditional merge compared the bare
// name, the graph split compared the whole address string.
TEST(XlingsIdentity, IsTheNamespaceAndNameAndNeverTheVersion) {
    EXPECT_EQ(addrset::package_key("xim:glibc@2.40"), "xim:glibc");
    EXPECT_EQ(addrset::package_key("xim:glibc"),      "xim:glibc");
    EXPECT_EQ(addrset::package_key("xim:glibc@>=2.38"), "xim:glibc");

    // The manifest's default namespace, spelled the same way `parse_xpkg_ref`
    // spells it: `ninja` and `xim:ninja` name one package.
    EXPECT_EQ(addrset::package_key("ninja"), "xim:ninja");

    // …and a different index is a different package, which the bare-name
    // definition could not express.
    EXPECT_NE(addrset::package_key("scode:cuda"), addrset::package_key("xim:cuda"));
}

// ─── Adjudication ──────────────────────────────────────────────────────────

// The arrangement section 6 of the design describes: the rule owns the floor,
// the project owns the exact version. Two statements, one install, and no
// warning -- a note here would fire on every build of every project that pins a
// tool its rule also requires, which is the intended arrangement.
TEST(XlingsUnify, OnePackageYieldsOneAddress) {
    std::vector<addrset::Claim> claims{
        project("xim:cuda-nvcc@13.3.33"),
        from("mcpp:plugins", "xim:cuda-nvcc@>=12.9.86"),
    };
    auto r = addrset::unify(claims);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(addresses(*r), (std::vector<std::string>{"xim:cuda-nvcc@13.3.33"}));
    EXPECT_TRUE(r->overrides.empty());
}

// …and when the two really do disagree, the note names both sides and says
// which was used. Reported, not inferred from an install log.
TEST(XlingsUnify, AResolvedDisagreementNamesBothSidesAndTheWinner) {
    std::vector<addrset::Claim> claims{
        project("xim:cuda-nvcc@13.3.33"),
        from("mcpp:plugins", "xim:cuda-nvcc@12.9.86"),
    };
    auto r = addrset::unify(claims);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->overrides.size(), 1u);
    EXPECT_NE(r->overrides[0].find("mcpp:plugins"), std::string::npos);
    EXPECT_NE(r->overrides[0].find("this project"), std::string::npos);
    EXPECT_NE(r->overrides[0].find("12.9.86"), std::string::npos);
    EXPECT_NE(r->overrides[0].find("13.3.33"), std::string::npos);
}

// A claim naming no version states that the package is wanted and nothing about
// which version, so it must not out-rank a floor merely by being nearer.
TEST(XlingsUnify, AnUnversionedClaimAbstainsFromTheVersionQuestion) {
    std::vector<addrset::Claim> claims{
        project("xim:cann-toolkit"),
        from("mcpp:plugins", "xim:cann-toolkit@>=8.5.0"),
    };
    auto r = addrset::unify(claims);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(addresses(*r),
              (std::vector<std::string>{"xim:cann-toolkit@>=8.5.0"}));
    EXPECT_TRUE(r->overrides.empty());
}

TEST(XlingsUnify, WithNoVersionAnywhereTheBareAddressSurvives) {
    std::vector<addrset::Claim> claims{
        project("xim:ninja"),
        from("board:virt", "ninja"),
    };
    auto r = addrset::unify(claims);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(addresses(*r), (std::vector<std::string>{"xim:ninja"}));
}

// Order is the order packages were first claimed, so a build's install list
// does not permute when an unrelated dependency is added.
TEST(XlingsUnify, KeepsFirstClaimedOrder) {
    std::vector<addrset::Claim> claims{
        project("xim:b@1.0"), project("xim:a@1.0"),
        from("dep", "xim:a@>=1.0"), from("dep", "xim:c@1.0"),
    };
    auto r = addrset::unify(claims);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(addresses(*r),
              (std::vector<std::string>{"xim:b@1.0", "xim:a@1.0", "xim:c@1.0"}));
}

// ─── Validation ────────────────────────────────────────────────────────────

// The one refusal this module adds. It is a comparison against a chosen
// version, not a search for one: nothing here consults which versions exist.
TEST(XlingsUnify, RefusesAPinBelowAStatedFloorAndNamesBothSides) {
    std::vector<addrset::Claim> claims{
        project("xim:cann-toolkit@8.0.0"),
        from("mcpp:plugins", "xim:cann-toolkit@>=8.5.0"),
    };
    auto r = addrset::unify(claims);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("xim:cann-toolkit"), std::string::npos);
    EXPECT_NE(r.error().find("8.0.0"), std::string::npos);
    EXPECT_NE(r.error().find(">=8.5.0"), std::string::npos);
    EXPECT_NE(r.error().find("mcpp:plugins"), std::string::npos);
    // The way out is in the message, which is what makes the shortcut of not
    // intersecting constraints an acceptable one.
    EXPECT_NE(r.error().find("drop the pin"), std::string::npos);
}

TEST(XlingsUnify, AcceptsAPinThatSatisfiesTheFloor) {
    std::vector<addrset::Claim> claims{
        project("xim:cann-toolkit@8.5.0"),
        from("mcpp:plugins", "xim:cann-toolkit@>=8.5.0"),
    };
    auto r = addrset::unify(claims);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(addresses(*r), (std::vector<std::string>{"xim:cann-toolkit@8.5.0"}));
    // Satisfied is silent: an override note here would fire on every build of
    // every project that pins a tool its rule also requires, which is the
    // intended arrangement rather than a problem.
    EXPECT_TRUE(r->overrides.empty());
}

// TWO EXACT PINS ARE TWO CHOICES, NOT A VIOLATED REQUIREMENT. Refusing them
// would turn every dependency that pinned a tool the project also pins into a
// hard failure on upgrade, for a disagreement adjudication already settles.
TEST(XlingsUnify, TwoExactPinsAreAdjudicatedRatherThanRefused) {
    std::vector<addrset::Claim> claims{
        project("xim:cuda-nvcc@13.3.33"),
        from("mcpp:plugins", "xim:cuda-nvcc@12.9.86"),
    };
    auto r = addrset::unify(claims);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(addresses(*r), (std::vector<std::string>{"xim:cuda-nvcc@13.3.33"}));
    EXPECT_EQ(r->overrides.size(), 1u);
}

// Two dependencies, no project statement: there is no "nearer", so the tie is
// broken by first appearance -- and the validation still runs, which is what
// keeps an arbitrary tie-break from silently lowering a floor.
TEST(XlingsUnify, TwoDependenciesInConflictAreStillValidated) {
    std::vector<addrset::Claim> ok{
        from("dep-a", "xim:tool@2.0.0"),
        from("dep-b", "xim:tool@>=1.0.0"),
    };
    ASSERT_TRUE(addrset::unify(ok).has_value());

    std::vector<addrset::Claim> bad{
        from("dep-a", "xim:tool@1.0.0"),
        from("dep-b", "xim:tool@>=2.0.0"),
    };
    auto r = addrset::unify(bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("dep-a"), std::string::npos);
    EXPECT_NE(r.error().find("dep-b"), std::string::npos);
}

// A version this grammar cannot parse is not evidence of a conflict. Refusing
// on a spelling nobody can evaluate would be a refusal manufactured from
// ignorance -- `8.0.RC1` is a real published version.
TEST(XlingsUnify, AnUnevaluableSpellingIsReportedNotRefused) {
    std::vector<addrset::Claim> claims{
        project("xim:cann-toolkit@8.0.RC1"),
        from("mcpp:plugins", "xim:cann-toolkit@>=8.5.0"),
    };
    auto r = addrset::unify(claims);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(addresses(*r), (std::vector<std::string>{"xim:cann-toolkit@8.0.RC1"}));
    EXPECT_EQ(r->overrides.size(), 1u);
}

// The same spelling twice is one statement, not a disagreement. Without this a
// project that is its own runtime owner would warn about itself on every build.
TEST(XlingsUnify, RepeatingOneSpellingIsSilent) {
    std::vector<addrset::Claim> claims{
        project("xim:shaderc@2026.3"),
        project("xim:shaderc@2026.3"),
        from("dep", "xim:shaderc@2026.3"),
    };
    auto r = addrset::unify(claims);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(addresses(*r), (std::vector<std::string>{"xim:shaderc@2026.3"}));
    EXPECT_TRUE(r->overrides.empty());
}
