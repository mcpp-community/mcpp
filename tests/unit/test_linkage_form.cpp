// Which form does a dependency take (issue #519)?
//
// The assertions are about the TABLE — its totality, and the identity of each
// cell — rather than about a build looking right afterwards. Two of them cover
// rules that were WRONG in the first draft of the design and were corrected
// against measurements, so they are regression tests for a decision as much as
// for code: the `-L` rule (§4.2) and the libc-linkage coupling (§12.3).

#include <gtest/gtest.h>

import std;
import mcpp.build.linkage_form;

namespace lf = mcpp::build::linkage_form;

namespace {

lf::PackageFacts source_package() {
    lf::PackageFacts p;
    p.label = "compat.zlib@1.3.2";
    p.hasSources = true;
    return p;
}

lf::TargetFacts hosted() { return lf::TargetFacts{}; }

lf::Request want(lf::DepLinkage linkage) {
    lf::Request r;
    r.whole = linkage;
    r.wholeIsExplicit = true;
    return r;
}

} // namespace

// ── the default is byte-for-byte the old behaviour ─────────────────────────

TEST(LinkageForm, DefaultRequestIsStaticAndSilent) {
    lf::Request none;                       // nobody wrote anything
    EXPECT_EQ(none.whole, lf::DepLinkage::Static);
    EXPECT_FALSE(none.wholeIsExplicit);

    auto pkg = source_package();
    auto answer = lf::resolve(pkg, lf::admissible(pkg, hosted()), none);
    EXPECT_EQ(answer.linkage, lf::DepLinkage::Static);
    EXPECT_TRUE(answer.diagnostic.empty());
}

TEST(LinkageForm, ASourcePackageCanBeEitherForm) {
    auto pkg = source_package();
    auto allowed = lf::admissible(pkg, hosted());
    EXPECT_TRUE(allowed.staticOk);
    EXPECT_TRUE(allowed.sharedOk);
    EXPECT_EQ(lf::resolve(pkg, allowed, want(lf::DepLinkage::Shared)).linkage,
              lf::DepLinkage::Shared);
}

// ── `kind = "shared"` is a constraint; `kind = "lib"` is not ───────────────

TEST(LinkageForm, DeclaredSharedIsAConstraintNotAPreference) {
    auto pkg = source_package();
    pkg.declaredShared = true;
    auto allowed = lf::admissible(pkg, hosted());
    EXPECT_FALSE(allowed.staticOk);
    EXPECT_TRUE(allowed.sharedOk);
    // Asking for static does not get it, and says so.
    auto answer = lf::resolve(pkg, allowed, want(lf::DepLinkage::Static));
    EXPECT_EQ(answer.linkage, lf::DepLinkage::Shared);
    EXPECT_FALSE(answer.diagnostic.empty());
}

TEST(LinkageForm, NotDeclaringSharedIsNotAConstraintToBeStatic) {
    // `kind = "lib"` is the parser's DEFAULT, written as boilerplate by 84 of
    // the 130 packages in mcpp-index. Reading it as "must be static" would
    // lock the whole ecosystem out of this axis, so absence must stay silent.
    auto pkg = source_package();
    ASSERT_FALSE(pkg.declaredShared);
    EXPECT_TRUE(lf::admissible(pkg, hosted()).sharedOk);
}

// ── the `-L` rule (round-two correction) ───────────────────────────────────

TEST(LinkageForm, ForeignLinkInputsAreRecognisedInEverySpelling) {
    EXPECT_TRUE(lf::carries_foreign_link_inputs(
        std::vector<std::string>{"-Llib", "-l:libssl.a"}));
    EXPECT_TRUE(lf::carries_foreign_link_inputs(
        std::vector<std::string>{"-L", "lib"}));
    EXPECT_TRUE(lf::carries_foreign_link_inputs(
        std::vector<std::string>{"-Wl,-Llib"}));
    EXPECT_TRUE(lf::carries_foreign_link_inputs(
        std::vector<std::string>{"/LIBPATH:lib"}));
}

TEST(LinkageForm, SystemLibrariesAreNotForeignLinkInputs) {
    // The 27 mcpp-index packages whose ldflags name only host libraries must
    // stay eligible: a shared object depending on libm is ordinary.
    EXPECT_FALSE(lf::carries_foreign_link_inputs(
        std::vector<std::string>{"-lm", "-lpthread", "-ldl", "-lrt"}));
    EXPECT_FALSE(lf::carries_foreign_link_inputs(
        std::vector<std::string>{"-lws2_32", "-lbcrypt", "-ladvapi32"}));
}

TEST(LinkageForm, APackageCarryingPrebuiltArchivesCannotBeShared) {
    // compat.openssl's shape: one anchor TU that mcpp compiles, plus `.a`
    // archives it ships. Wrapping somebody else's non-PIC archive in a shared
    // object is not something mcpp can do, so the request is refused WITH a
    // reason naming the cause.
    auto pkg = source_package();
    pkg.label = "compat.openssl@3.5.0";
    pkg.carriesForeignLinkInputs = true;
    auto allowed = lf::admissible(pkg, hosted());
    EXPECT_TRUE(allowed.staticOk);
    EXPECT_FALSE(allowed.sharedOk);
    EXPECT_NE(allowed.sharedRefusal.find("-L"), std::string::npos);

    auto answer = lf::resolve(pkg, allowed, want(lf::DepLinkage::Shared));
    EXPECT_EQ(answer.linkage, lf::DepLinkage::Static);
    EXPECT_FALSE(answer.diagnostic.empty());
}

TEST(LinkageForm, APackageWithNoSourcesOfItsOwnCannotBeShared) {
    lf::PackageFacts pkg;
    pkg.label = "vendor.glib@2.80.0";
    pkg.hasSources = false;
    EXPECT_FALSE(lf::admissible(pkg, hosted()).sharedOk);
}

// ── the target's own veto (round-two correction) ───────────────────────────

TEST(LinkageForm, AFreestandingTargetHasNothingToLoadASharedLibraryWith) {
    auto pkg = source_package();
    lf::TargetFacts bare;
    bare.hasLoader = false;
    auto allowed = lf::admissible(pkg, bare);
    EXPECT_FALSE(allowed.sharedOk);
    EXPECT_EQ(lf::resolve(pkg, allowed, want(lf::DepLinkage::Shared)).linkage,
              lf::DepLinkage::Static);
}

TEST(LinkageForm, AFullyStaticLibcVetoesTheSharedForm) {
    // The two axes named `linkage` are NOT independent. A `-static` image
    // has no interpreter, so it cannot load a shared object — and musl
    // defaults to `linkage = "static"`, which makes this the COMMON path
    // there rather than a corner.
    auto pkg = source_package();
    lf::TargetFacts staticLibc;
    staticLibc.fullStaticLibc = true;
    auto allowed = lf::admissible(pkg, staticLibc);
    EXPECT_FALSE(allowed.sharedOk);
    EXPECT_NE(allowed.sharedRefusal.find("static"), std::string::npos);

    auto answer = lf::resolve(pkg, allowed, want(lf::DepLinkage::Shared));
    EXPECT_EQ(answer.linkage, lf::DepLinkage::Static);
    // It must SAY why, or the user edits the wrong key forever.
    EXPECT_FALSE(answer.diagnostic.empty());
}

TEST(LinkageForm, TheTargetVetoIsReportedBeforeThePackageOne) {
    // Order is the order of the diagnostic: a reason the user cannot fix by
    // editing the package must not point at the package.
    lf::PackageFacts pkg = source_package();
    pkg.carriesForeignLinkInputs = true;
    lf::TargetFacts bare;
    bare.hasLoader = false;
    auto allowed = lf::admissible(pkg, bare);
    EXPECT_EQ(allowed.sharedRefusal.find("compat.zlib"), std::string::npos);
}

// ── distribution packages answer from what they ship ───────────────────────

TEST(LinkageForm, ADistributionPackageOffersOnlyTheLegsItShips) {
    lf::PackageFacts pkg;
    pkg.label = "acme.mathkit@0.1.0";
    pkg.isDistribution = true;
    pkg.shipsStatic = true;
    auto allowed = lf::admissible(pkg, hosted());
    EXPECT_TRUE(allowed.staticOk);
    EXPECT_FALSE(allowed.sharedOk);
    EXPECT_NE(allowed.sharedRefusal.find("mathkit"), std::string::npos);

    pkg.shipsShared = true;
    EXPECT_TRUE(lf::admissible(pkg, hosted()).sharedOk);
}

// ── per-package request ────────────────────────────────────────────────────

TEST(LinkageForm, APerPackageRequestOverridesTheWholeGraphDefault) {
    auto pkg = source_package();
    lf::Request request;                       // default static, not explicit
    request.perPackage["compat.zlib@1.3.2"] = lf::DepLinkage::Shared;
    EXPECT_EQ(lf::resolve(pkg, lf::admissible(pkg, hosted()), request).linkage,
              lf::DepLinkage::Shared);
}

TEST(LinkageForm, AnUnaskedForRefusalStaysSilent) {
    // mcpp promised nothing when nobody asked. A default that cannot be
    // honoured is not a broken promise, and warning about it on every build
    // puts noise on correct manifests.
    auto pkg = source_package();
    pkg.declaredShared = true;                 // constrained to shared
    lf::Request none;                          // default static, NOT explicit
    auto answer = lf::resolve(pkg, lf::admissible(pkg, hosted()), none);
    EXPECT_EQ(answer.linkage, lf::DepLinkage::Shared);
    EXPECT_TRUE(answer.diagnostic.empty());
}

// ── PIC ────────────────────────────────────────────────────────────────────

TEST(LinkageForm, PicFollowsAnySharedFormInTheGraph) {
    using L = lf::DepLinkage;
    std::vector<L> allStatic{L::Static, L::Static};
    std::vector<L> oneShared{L::Static, L::Shared};
    EXPECT_FALSE(lf::needs_pic(allStatic, /*anyOwnSharedTarget=*/false));
    EXPECT_TRUE(lf::needs_pic(allStatic,  /*anyOwnSharedTarget=*/true));
    EXPECT_TRUE(lf::needs_pic(oneShared,  /*anyOwnSharedTarget=*/false));
}

// ── vocabulary ─────────────────────────────────────────────────────────────

TEST(LinkageForm, TheVocabularyIsClosed) {
    EXPECT_EQ(lf::parse("static"), lf::DepLinkage::Static);
    EXPECT_EQ(lf::parse("shared"), lf::DepLinkage::Shared);
    EXPECT_FALSE(lf::parse("dynamic").has_value());   // the libc axis' word
    EXPECT_FALSE(lf::parse("").has_value());
    EXPECT_EQ(lf::to_string(lf::DepLinkage::Static), "static");
    EXPECT_EQ(lf::to_string(lf::DepLinkage::Shared), "shared");
}
