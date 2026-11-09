// Which form does a dependency take (issue #519)?
//
// The assertions are about the TABLE — its totality, and the identity of each
// cell — rather than about a build looking right afterwards. Two of them cover
// rules that were WRONG in the first draft of the design and were corrected
// against measurements, so they are regression tests for a decision as much as
// for code: the `-L` rule (§4.2) and the libc-linkage coupling (§12.3).


import std;
import mcpplibs.mtest;
import mcpp.build.linkage_form;

using namespace mcpplibs::mtest;

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

UTest default_request_is_static_and_silent{"LinkageForm/DefaultRequestIsStaticAndSilent", [] {
    lf::Request none;                       // nobody wrote anything
    expect(eq(none.whole, lf::DepLinkage::Static));
    expect(!none.wholeIsExplicit);

    auto pkg = source_package();
    auto answer = lf::resolve(pkg, lf::admissible(pkg, hosted()), none);
    expect(eq(answer.linkage, lf::DepLinkage::Static));
    expect(answer.diagnostic.empty());
}};

UTest a_source_package_can_be_either_form{"LinkageForm/ASourcePackageCanBeEitherForm", [] {
    auto pkg = source_package();
    auto allowed = lf::admissible(pkg, hosted());
    expect(bool(allowed.staticOk));
    expect(bool(allowed.sharedOk));
    expect(eq(lf::resolve(pkg, allowed, want(lf::DepLinkage::Shared)).linkage, lf::DepLinkage::Shared));
}};

// ── `kind = "shared"` is a constraint; `kind = "lib"` is not ───────────────

UTest declared_shared_is_a_constraint_not_a_preference{"LinkageForm/DeclaredSharedIsAConstraintNotAPreference", [] {
    auto pkg = source_package();
    pkg.declaredShared = true;
    auto allowed = lf::admissible(pkg, hosted());
    expect(!allowed.staticOk);
    expect(bool(allowed.sharedOk));
    // Asking for static does not get it, and says so.
    auto answer = lf::resolve(pkg, allowed, want(lf::DepLinkage::Static));
    expect(eq(answer.linkage, lf::DepLinkage::Shared));
    expect(!answer.diagnostic.empty());
}};

UTest not_declaring_shared_is_not_a_constraint_to_be_static{"LinkageForm/NotDeclaringSharedIsNotAConstraintToBeStatic", [] {
    // `kind = "lib"` is the parser's DEFAULT, written as boilerplate by 84 of
    // the 130 packages in mcpp-index. Reading it as "must be static" would
    // lock the whole ecosystem out of this axis, so absence must stay silent.
    auto pkg = source_package();
    expect(fatal(!pkg.declaredShared));
    expect(bool(lf::admissible(pkg, hosted()).sharedOk));
}};

// ── the `-L` rule (round-two correction) ───────────────────────────────────

UTest foreign_link_inputs_are_recognised_in_every_spelling{"LinkageForm/ForeignLinkInputsAreRecognisedInEverySpelling", [] {
    expect(bool(lf::carries_foreign_link_inputs(
        std::vector<std::string>{"-Llib", "-l:libssl.a"})));
    expect(bool(lf::carries_foreign_link_inputs(
        std::vector<std::string>{"-L", "lib"})));
    expect(bool(lf::carries_foreign_link_inputs(
        std::vector<std::string>{"-Wl,-Llib"})));
    expect(bool(lf::carries_foreign_link_inputs(
        std::vector<std::string>{"/LIBPATH:lib"})));
}};

UTest system_libraries_are_not_foreign_link_inputs{"LinkageForm/SystemLibrariesAreNotForeignLinkInputs", [] {
    // The 27 mcpp-index packages whose ldflags name only host libraries must
    // stay eligible: a shared object depending on libm is ordinary.
    expect(!lf::carries_foreign_link_inputs(
        std::vector<std::string>{"-lm", "-lpthread", "-ldl", "-lrt"}));
    expect(!lf::carries_foreign_link_inputs(
        std::vector<std::string>{"-lws2_32", "-lbcrypt", "-ladvapi32"}));
}};

UTest a_package_carrying_prebuilt_archives_cannot_be_shared{"LinkageForm/APackageCarryingPrebuiltArchivesCannotBeShared", [] {
    // compat.openssl's shape: one anchor TU that mcpp compiles, plus `.a`
    // archives it ships. Wrapping somebody else's non-PIC archive in a shared
    // object is not something mcpp can do, so the request is refused WITH a
    // reason naming the cause.
    auto pkg = source_package();
    pkg.label = "compat.openssl@3.5.0";
    pkg.carriesForeignLinkInputs = true;
    auto allowed = lf::admissible(pkg, hosted());
    expect(bool(allowed.staticOk));
    expect(!allowed.sharedOk);
    expect(ne(allowed.sharedRefusal.find("-L"), std::string::npos));

    auto answer = lf::resolve(pkg, allowed, want(lf::DepLinkage::Shared));
    expect(eq(answer.linkage, lf::DepLinkage::Static));
    expect(!answer.diagnostic.empty());
}};

UTest a_package_with_no_sources_of_its_own_cannot_be_shared{"LinkageForm/APackageWithNoSourcesOfItsOwnCannotBeShared", [] {
    lf::PackageFacts pkg;
    pkg.label = "vendor.glib@2.80.0";
    pkg.hasSources = false;
    expect(!(lf::admissible(pkg, hosted()).sharedOk));
}};

// ── the target's own veto (round-two correction) ───────────────────────────

UTest a_freestanding_target_has_nothing_to_load_a_shared_library_with{"LinkageForm/AFreestandingTargetHasNothingToLoadASharedLibraryWith", [] {
    auto pkg = source_package();
    lf::TargetFacts bare;
    bare.hasLoader = false;
    auto allowed = lf::admissible(pkg, bare);
    expect(!allowed.sharedOk);
    expect(eq(lf::resolve(pkg, allowed, want(lf::DepLinkage::Shared)).linkage, lf::DepLinkage::Static));
}};

UTest a_fully_static_libc_vetoes_the_shared_form{"LinkageForm/AFullyStaticLibcVetoesTheSharedForm", [] {
    // The two axes named `linkage` are NOT independent. A `-static` image
    // has no interpreter, so it cannot load a shared object — and musl
    // defaults to `linkage = "static"`, which makes this the COMMON path
    // there rather than a corner.
    auto pkg = source_package();
    lf::TargetFacts staticLibc;
    staticLibc.fullStaticLibc = true;
    auto allowed = lf::admissible(pkg, staticLibc);
    expect(!allowed.sharedOk);
    expect(ne(allowed.sharedRefusal.find("static"), std::string::npos));

    auto answer = lf::resolve(pkg, allowed, want(lf::DepLinkage::Shared));
    expect(eq(answer.linkage, lf::DepLinkage::Static));
    // It must SAY why, or the user edits the wrong key forever.
    expect(!answer.diagnostic.empty());
}};

UTest the_target_veto_is_reported_before_the_package_one{"LinkageForm/TheTargetVetoIsReportedBeforeThePackageOne", [] {
    // Order is the order of the diagnostic: a reason the user cannot fix by
    // editing the package must not point at the package.
    lf::PackageFacts pkg = source_package();
    pkg.carriesForeignLinkInputs = true;
    lf::TargetFacts bare;
    bare.hasLoader = false;
    auto allowed = lf::admissible(pkg, bare);
    expect(eq(allowed.sharedRefusal.find("compat.zlib"), std::string::npos));
}};

// ── distribution packages answer from what they ship ───────────────────────

UTest a_distribution_package_offers_only_the_legs_it_ships{"LinkageForm/ADistributionPackageOffersOnlyTheLegsItShips", [] {
    lf::PackageFacts pkg;
    pkg.label = "acme.mathkit@0.1.0";
    pkg.isDistribution = true;
    pkg.shipsStatic = true;
    auto allowed = lf::admissible(pkg, hosted());
    expect(bool(allowed.staticOk));
    expect(!allowed.sharedOk);
    expect(ne(allowed.sharedRefusal.find("mathkit"), std::string::npos));

    pkg.shipsShared = true;
    expect(bool(lf::admissible(pkg, hosted()).sharedOk));
}};

// ── per-package request ────────────────────────────────────────────────────

UTest a_per_package_request_overrides_the_whole_graph_default{"LinkageForm/APerPackageRequestOverridesTheWholeGraphDefault", [] {
    auto pkg = source_package();
    lf::Request request;                       // default static, not explicit
    request.perPackage["compat.zlib@1.3.2"] = lf::DepLinkage::Shared;
    expect(eq(lf::resolve(pkg, lf::admissible(pkg, hosted()), request).linkage, lf::DepLinkage::Shared));
}};

UTest an_unasked_for_refusal_stays_silent{"LinkageForm/AnUnaskedForRefusalStaysSilent", [] {
    // mcpp promised nothing when nobody asked. A default that cannot be
    // honoured is not a broken promise, and warning about it on every build
    // puts noise on correct manifests.
    auto pkg = source_package();
    pkg.declaredShared = true;                 // constrained to shared
    lf::Request none;                          // default static, NOT explicit
    auto answer = lf::resolve(pkg, lf::admissible(pkg, hosted()), none);
    expect(eq(answer.linkage, lf::DepLinkage::Shared));
    expect(answer.diagnostic.empty());
}};

// ── PIC ────────────────────────────────────────────────────────────────────

UTest pic_follows_any_shared_form_in_the_graph{"LinkageForm/PicFollowsAnySharedFormInTheGraph", [] {
    using L = lf::DepLinkage;
    std::vector<L> allStatic{L::Static, L::Static};
    std::vector<L> oneShared{L::Static, L::Shared};
    expect(!lf::needs_pic(allStatic, /*anyOwnSharedTarget=*/false));
    expect(bool(lf::needs_pic(allStatic,  /*anyOwnSharedTarget=*/true)));
    expect(bool(lf::needs_pic(oneShared,  /*anyOwnSharedTarget=*/false)));
}};

// ── vocabulary ─────────────────────────────────────────────────────────────

UTest the_vocabulary_is_closed{"LinkageForm/TheVocabularyIsClosed", [] {
    expect(eq(lf::parse("static"), lf::DepLinkage::Static));
    expect(eq(lf::parse("shared"), lf::DepLinkage::Shared));
    expect(!(lf::parse("dynamic").has_value()));   // the libc axis' word
    expect(!(lf::parse("").has_value()));
    expect(eq(lf::to_string(lf::DepLinkage::Static), "static"));
    expect(eq(lf::to_string(lf::DepLinkage::Shared), "shared"));
}};

// ── a package's default form (#642 E1) ─────────────────────────────────────

namespace {

lf::PackageFacts defaulting_to(lf::DepLinkage form) {
    auto pkg = source_package();
    pkg.defaultLinkage = form;
    pkg.defaultDeclaredBy = std::format("[targets.zlib] linkage = \"{}\"",
                                        lf::to_string(form));
    return pkg;
}

} // namespace

UTest a_package_default_is_answered_when_nobody_asks{"LinkageForm/APackageDefaultIsAnsweredWhenNobodyAsks", [] {
    auto pkg = defaulting_to(lf::DepLinkage::Shared);
    auto answer = lf::resolve(pkg, lf::admissible(pkg, hosted()), lf::Request{});
    expect(eq(answer.linkage, lf::DepLinkage::Shared));
    expect(eq(answer.reason, "package-default"));
    expect(answer.diagnostic.empty());
    expect(answer.note.empty());
}};

UTest a_package_default_is_not_a_constraint{"LinkageForm/APackageDefaultIsNotAConstraint", [] {
    // The difference from `kind = "shared"` is the whole feature: both forms
    // stay admissible, so a request for the other one is not refused.
    auto pkg = defaulting_to(lf::DepLinkage::Shared);
    auto allowed = lf::admissible(pkg, hosted());
    expect(bool(allowed.staticOk));
    expect(bool(allowed.sharedOk));
    expect(allowed.constraint.empty());
}};

UTest an_edge_request_overrides_the_package_default_and_names_both_statements{"LinkageForm/AnEdgeRequestOverridesThePackageDefaultAndNamesBothStatements", [] {
    auto pkg = defaulting_to(lf::DepLinkage::Shared);
    lf::Request request;
    request.perPackage[pkg.label] = lf::DepLinkage::Static;
    auto answer = lf::resolve(pkg, lf::admissible(pkg, hosted()), request);
    expect(eq(answer.linkage, lf::DepLinkage::Static));
    expect(eq(answer.reason, "requested"));
    // Honoured, so not a degradation: `--strict` must accept it.
    expect(answer.diagnostic.empty());
    expect(ne(answer.note.find("`linkage = \"static\"` on this dependency"), std::string::npos)) << answer.note;
    expect(ne(answer.note.find("[targets.zlib] linkage = \"shared\""), std::string::npos)) << answer.note;
}};

UTest a_written_whole_graph_value_outranks_the_package_default{"LinkageForm/AWrittenWholeGraphValueOutranksThePackageDefault", [] {
    auto pkg = defaulting_to(lf::DepLinkage::Shared);
    auto answer = lf::resolve(pkg, lf::admissible(pkg, hosted()),
                              want(lf::DepLinkage::Static));
    expect(eq(answer.linkage, lf::DepLinkage::Static));
    expect(eq(answer.reason, "requested"));
    expect(ne(answer.note.find("dependency_linkage = \"static\""), std::string::npos))
        << answer.note;
}};

UTest an_unwritten_whole_graph_value_does_not_outrank_the_package_default{"LinkageForm/AnUnwrittenWholeGraphValueDoesNotOutrankThePackageDefault", [] {
    // `whole` is static by construction; only a human writing it makes it a
    // statement, and mcpp's own default is exactly what a package default
    // replaces.
    auto pkg = defaulting_to(lf::DepLinkage::Shared);
    lf::Request none;
    expect(fatal(eq(none.whole, lf::DepLinkage::Static)));
    expect(eq(lf::resolve(pkg, lf::admissible(pkg, hosted()), none).linkage, lf::DepLinkage::Shared));
}};

UTest a_request_that_agrees_with_the_default_prints_nothing{"LinkageForm/ARequestThatAgreesWithTheDefaultPrintsNothing", [] {
    auto pkg = defaulting_to(lf::DepLinkage::Shared);
    lf::Request request;
    request.perPackage[pkg.label] = lf::DepLinkage::Shared;
    auto answer = lf::resolve(pkg, lf::admissible(pkg, hosted()), request);
    expect(eq(answer.reason, "requested"));
    expect(answer.note.empty());
}};

UTest a_default_the_target_cannot_honour_falls_back_without_a_word{"LinkageForm/ADefaultTheTargetCannotHonourFallsBackWithoutAWord", [] {
    // Nobody asked, so there is no broken promise to report: the default is
    // the package's preference, and a fully static image cannot load a
    // shared library at all.
    auto pkg = defaulting_to(lf::DepLinkage::Shared);
    lf::TargetFacts target;
    target.fullStaticLibc = true;
    auto answer = lf::resolve(pkg, lf::admissible(pkg, target), lf::Request{});
    expect(eq(answer.linkage, lf::DepLinkage::Static));
    expect(eq(answer.reason, "static-libc"));
    expect(answer.diagnostic.empty());
    expect(answer.note.empty());
}};
