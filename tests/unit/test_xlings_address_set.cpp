
import std;
import mcpplibs.mtest;
import mcpp.provider.address_set;

using namespace mcpplibs::mtest;

namespace addrset = mcpp::provider::addrset;

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
UTest is_the_namespace_and_name_and_never_the_version{"XlingsIdentity/IsTheNamespaceAndNameAndNeverTheVersion", [] {
    expect(eq(addrset::package_key("xim:glibc@2.40"), "xim:glibc"));
    expect(eq(addrset::package_key("xim:glibc"), "xim:glibc"));
    expect(eq(addrset::package_key("xim:glibc@>=2.38"), "xim:glibc"));

    // The manifest's default namespace, spelled the same way `parse_xpkg_ref`
    // spells it: `ninja` and `xim:ninja` name one package.
    expect(eq(addrset::package_key("ninja"), "xim:ninja"));

    // …and a different index is a different package, which the bare-name
    // definition could not express.
    expect(ne(addrset::package_key("scode:cuda"), addrset::package_key("xim:cuda")));
}};

// ─── Adjudication ──────────────────────────────────────────────────────────

// The arrangement section 6 of the design describes: the rule owns the floor,
// the project owns the exact version. Two statements, one install, and no
// warning -- a note here would fire on every build of every project that pins a
// tool its rule also requires, which is the intended arrangement.
UTest one_package_yields_one_address{"XlingsUnify/OnePackageYieldsOneAddress", [] {
    std::vector<addrset::Claim> claims{
        project("xim:cuda-nvcc@13.3.33"),
        from("mcpp:plugins", "xim:cuda-nvcc@>=12.9.86"),
    };
    auto r = addrset::unify(claims);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(eq(addresses(*r), (std::vector<std::string>{"xim:cuda-nvcc@13.3.33"})));
    expect(r->overrides.empty());
}};

// …and when the two really do disagree, the note names both sides and says
// which was used. Reported, not inferred from an install log.
UTest a_resolved_disagreement_names_both_sides_and_the_winner{"XlingsUnify/AResolvedDisagreementNamesBothSidesAndTheWinner", [] {
    std::vector<addrset::Claim> claims{
        project("xim:cuda-nvcc@13.3.33"),
        from("mcpp:plugins", "xim:cuda-nvcc@12.9.86"),
    };
    auto r = addrset::unify(claims);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(fatal(eq(r->overrides.size(), 1u)));
    expect(ne(r->overrides[0].find("mcpp:plugins"), std::string::npos));
    expect(ne(r->overrides[0].find("this project"), std::string::npos));
    expect(ne(r->overrides[0].find("12.9.86"), std::string::npos));
    expect(ne(r->overrides[0].find("13.3.33"), std::string::npos));
}};

// A claim naming no version states that the package is wanted and nothing about
// which version, so it must not out-rank a floor merely by being nearer.
UTest an_unversioned_claim_abstains_from_the_version_question{"XlingsUnify/AnUnversionedClaimAbstainsFromTheVersionQuestion", [] {
    std::vector<addrset::Claim> claims{
        project("xim:cann-toolkit"),
        from("mcpp:plugins", "xim:cann-toolkit@>=8.5.0"),
    };
    auto r = addrset::unify(claims);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(eq(addresses(*r), (std::vector<std::string>{"xim:cann-toolkit@>=8.5.0"})));
    expect(r->overrides.empty());
}};

UTest with_no_version_anywhere_the_bare_address_survives{"XlingsUnify/WithNoVersionAnywhereTheBareAddressSurvives", [] {
    std::vector<addrset::Claim> claims{
        project("xim:ninja"),
        from("board:virt", "ninja"),
    };
    auto r = addrset::unify(claims);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(eq(addresses(*r), (std::vector<std::string>{"xim:ninja"})));
}};

// Order is the order packages were first claimed, so a build's install list
// does not permute when an unrelated dependency is added.
UTest keeps_first_claimed_order{"XlingsUnify/KeepsFirstClaimedOrder", [] {
    std::vector<addrset::Claim> claims{
        project("xim:b@1.0"), project("xim:a@1.0"),
        from("dep", "xim:a@>=1.0"), from("dep", "xim:c@1.0"),
    };
    auto r = addrset::unify(claims);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(eq(addresses(*r), (std::vector<std::string>{"xim:b@1.0", "xim:a@1.0", "xim:c@1.0"})));
}};

// ─── Validation ────────────────────────────────────────────────────────────

// The one refusal this module adds. It is a comparison against a chosen
// version, not a search for one: nothing here consults which versions exist.
UTest refuses_a_pin_below_a_stated_floor_and_names_both_sides{"XlingsUnify/RefusesAPinBelowAStatedFloorAndNamesBothSides", [] {
    std::vector<addrset::Claim> claims{
        project("xim:cann-toolkit@8.0.0"),
        from("mcpp:plugins", "xim:cann-toolkit@>=8.5.0"),
    };
    auto r = addrset::unify(claims);
    expect(fatal(!r.has_value()));
    expect(ne(r.error().find("xim:cann-toolkit"), std::string::npos));
    expect(ne(r.error().find("8.0.0"), std::string::npos));
    expect(ne(r.error().find(">=8.5.0"), std::string::npos));
    expect(ne(r.error().find("mcpp:plugins"), std::string::npos));
    // The way out is in the message, which is what makes the shortcut of not
    // intersecting constraints an acceptable one.
    expect(ne(r.error().find("drop the pin"), std::string::npos));
}};

UTest accepts_a_pin_that_satisfies_the_floor{"XlingsUnify/AcceptsAPinThatSatisfiesTheFloor", [] {
    std::vector<addrset::Claim> claims{
        project("xim:cann-toolkit@8.5.0"),
        from("mcpp:plugins", "xim:cann-toolkit@>=8.5.0"),
    };
    auto r = addrset::unify(claims);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(eq(addresses(*r), (std::vector<std::string>{"xim:cann-toolkit@8.5.0"})));
    // Satisfied is silent: an override note here would fire on every build of
    // every project that pins a tool its rule also requires, which is the
    // intended arrangement rather than a problem.
    expect(r->overrides.empty());
}};

// TWO EXACT PINS ARE TWO CHOICES, NOT A VIOLATED REQUIREMENT. Refusing them
// would turn every dependency that pinned a tool the project also pins into a
// hard failure on upgrade, for a disagreement adjudication already settles.
UTest two_exact_pins_are_adjudicated_rather_than_refused{"XlingsUnify/TwoExactPinsAreAdjudicatedRatherThanRefused", [] {
    std::vector<addrset::Claim> claims{
        project("xim:cuda-nvcc@13.3.33"),
        from("mcpp:plugins", "xim:cuda-nvcc@12.9.86"),
    };
    auto r = addrset::unify(claims);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(eq(addresses(*r), (std::vector<std::string>{"xim:cuda-nvcc@13.3.33"})));
    expect(eq(r->overrides.size(), 1u));
}};

// Two dependencies, no project statement: there is no "nearer", so the tie is
// broken by first appearance -- and the validation still runs, which is what
// keeps an arbitrary tie-break from silently lowering a floor.
UTest two_dependencies_in_conflict_are_still_validated{"XlingsUnify/TwoDependenciesInConflictAreStillValidated", [] {
    std::vector<addrset::Claim> ok{
        from("dep-a", "xim:tool@2.0.0"),
        from("dep-b", "xim:tool@>=1.0.0"),
    };
    expect(fatal(addrset::unify(ok).has_value()));

    std::vector<addrset::Claim> bad{
        from("dep-a", "xim:tool@1.0.0"),
        from("dep-b", "xim:tool@>=2.0.0"),
    };
    auto r = addrset::unify(bad);
    expect(fatal(!r.has_value()));
    expect(ne(r.error().find("dep-a"), std::string::npos));
    expect(ne(r.error().find("dep-b"), std::string::npos));
}};

// A version this grammar cannot parse is not evidence of a conflict. Refusing
// on a spelling nobody can evaluate would be a refusal manufactured from
// ignorance -- `8.0.RC1` is a real published version.
UTest an_unevaluable_spelling_is_reported_not_refused{"XlingsUnify/AnUnevaluableSpellingIsReportedNotRefused", [] {
    std::vector<addrset::Claim> claims{
        project("xim:cann-toolkit@8.0.RC1"),
        from("mcpp:plugins", "xim:cann-toolkit@>=8.5.0"),
    };
    auto r = addrset::unify(claims);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(eq(addresses(*r), (std::vector<std::string>{"xim:cann-toolkit@8.0.RC1"})));
    expect(eq(r->overrides.size(), 1u));
}};

// The same spelling twice is one statement, not a disagreement. Without this a
// project that is its own runtime owner would warn about itself on every build.
UTest repeating_one_spelling_is_silent{"XlingsUnify/RepeatingOneSpellingIsSilent", [] {
    std::vector<addrset::Claim> claims{
        project("xim:shaderc@2026.3"),
        project("xim:shaderc@2026.3"),
        from("dep", "xim:shaderc@2026.3"),
    };
    auto r = addrset::unify(claims);
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error();
    expect(eq(addresses(*r), (std::vector<std::string>{"xim:shaderc@2026.3"})));
    expect(r->overrides.empty());
}};
