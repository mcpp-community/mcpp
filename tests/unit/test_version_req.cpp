
import std;
import mcpplibs.mtest;
import mcpp.version_req;

using namespace mcpplibs::mtest;

using namespace mcpp::version_req;

UTest version_req_parse_version{"VersionReq/ParseVersion", [] {
    auto v = parse_version("1.2.3");
    expect(fatal(bool(v)));
    expect(eq(v->major(), 1)); expect(eq(v->minor(), 2)); expect(eq(v->patch(), 3));

    v = parse_version("0.5");
    expect(fatal(bool(v)));
    expect(eq(v->major(), 0)); expect(eq(v->minor(), 5)); expect(eq(v->patch(), 0));

    v = parse_version("7");
    expect(fatal(bool(v)));
    expect(eq(v->major(), 7)); expect(eq(v->minor(), 0)); expect(eq(v->patch(), 0));

    // #363: pre-release is PARSED, not stripped. Build metadata is dropped from
    // the order (SemVer §10) but the key's identity lives in the literal, which
    // pm/resolver.cppm carries separately.
    v = parse_version("1.2.3-beta.1+build42");
    expect(fatal(bool(v)));
    expect(eq(v->str(), "1.2.3-beta.1"));
    expect(bool(v->isPrerelease()));
}};

UTest parse_any{"VersionReq/ParseAny", [] {
    expect(bool(parse_req("")->any));
    expect(bool(parse_req("*")->any));
    expect(bool(parse_req("  *  ")->any));
}};

UTest match_exact{"VersionReq/MatchExact", [] {
    auto r = parse_req("=1.2.3");  expect(fatal(bool(r)));
    expect(matches(*r, *parse_version("1.2.3")));
    expect(!(matches(*r, *parse_version("1.2.4"))));
    expect(!(matches(*r, *parse_version("1.2.2"))));
}};

UTest match_caret_bare{"VersionReq/MatchCaretBare", [] {
    auto r = parse_req("1.2.3");   expect(fatal(bool(r)));
    expect(matches(*r, *parse_version("1.2.3")));
    expect(matches(*r, *parse_version("1.5.0")));
    expect(matches(*r, *parse_version("1.99.99")));
    expect(!(matches(*r, *parse_version("2.0.0"))));
    expect(!(matches(*r, *parse_version("1.2.2"))));
}};

UTest match_caret_zero_major{"VersionReq/MatchCaretZeroMajor", [] {
    auto r = parse_req("0.5.3");   expect(fatal(bool(r)));
    expect(matches(*r, *parse_version("0.5.3")));
    expect(matches(*r, *parse_version("0.5.99")));
    expect(!(matches(*r, *parse_version("0.6.0")))) << "minor bump kills caret when major==0";
}};

UTest match_tilde{"VersionReq/MatchTilde", [] {
    auto r = parse_req("~1.2.3");  expect(fatal(bool(r)));
    expect(matches(*r, *parse_version("1.2.3")));
    expect(matches(*r, *parse_version("1.2.99")));
    expect(!(matches(*r, *parse_version("1.3.0"))));
    expect(!(matches(*r, *parse_version("1.2.2"))));
}};

// A segment that is not written is a wildcard, not a zero (Cargo's partial
// requirements). Reading `^0` as `^0.0.0` made it admit 0.0.0 alone.
UTest partial_caret_bounds_follow_the_written_segments{"VersionReq/PartialCaretBoundsFollowTheWrittenSegments", [] {
    auto major = parse_req("^0");     expect(fatal(bool(major)));
    expect(matches(*major, *parse_version("0.0.0")));
    expect(matches(*major, *parse_version("0.0.5")));
    expect(matches(*major, *parse_version("0.9.3")));
    expect(!(matches(*major, *parse_version("1.0.0"))));

    auto minor = parse_req("^0.0");   expect(fatal(bool(minor)));
    expect(matches(*minor, *parse_version("0.0.0")));
    expect(matches(*minor, *parse_version("0.0.7")));
    expect(!(matches(*minor, *parse_version("0.1.0"))));

    // Every segment written: the leftmost-nonzero rule is unchanged.
    auto patch = parse_req("^0.0.0"); expect(fatal(bool(patch)));
    expect(matches(*patch, *parse_version("0.0.0")));
    expect(!(matches(*patch, *parse_version("0.0.1"))));

    auto exact = parse_req("^0.0.3"); expect(fatal(bool(exact)));
    expect(matches(*exact, *parse_version("0.0.3")));
    expect(!(matches(*exact, *parse_version("0.0.4"))));

    auto one = parse_req("^1");       expect(fatal(bool(one)));
    expect(matches(*one, *parse_version("1.9.9")));
    expect(!(matches(*one, *parse_version("2.0.0"))));

    auto zeroFive = parse_req("^0.5"); expect(fatal(bool(zeroFive)));
    expect(matches(*zeroFive, *parse_version("0.5.9")));
    expect(!(matches(*zeroFive, *parse_version("0.6.0"))));
}};

// A bare requirement is a caret (#866's follow-up), so a bare partial spelling
// takes the same wildcard bounds: "0" is <1.0.0 and "0.0" is <0.1.0, where
// before #866 both matched 0.0.0 alone.
UTest bare_partial_requirements_are_carets{"VersionReq/BarePartialRequirementsAreCarets", [] {
    for (auto [bare, caret] : {std::pair{"0", "^0"}, std::pair{"0.0", "^0.0"},
                               std::pair{"1", "^1"}, std::pair{"1.2", "^1.2"}}) {
        auto b = parse_req(bare);  expect(fatal(bool(b))) << bare;
        auto c = parse_req(caret); expect(fatal(bool(c))) << caret;
        for (auto v : {"0.0.0", "0.0.7", "0.1.0", "0.9.3", "1.0.0", "1.2.0", "1.2.9", "1.3.0", "2.0.0"})
            expect(eq(matches(*b, *parse_version(v)), matches(*c, *parse_version(v))))
                << bare << " against " << v;
    }
    expect(matches(*parse_req("0"), *parse_version("0.9.3")));
    expect(!(matches(*parse_req("0"), *parse_version("1.0.0"))));
    expect(matches(*parse_req("0.0"), *parse_version("0.0.7")));
    expect(!(matches(*parse_req("0.0"), *parse_version("0.1.0"))));
}};

UTest partial_tilde_with_only_a_major_holds_the_major{"VersionReq/PartialTildeWithOnlyAMajorHoldsTheMajor", [] {
    auto r = parse_req("~1");   expect(fatal(bool(r)));
    expect(matches(*r, *parse_version("1.0.0")));
    expect(matches(*r, *parse_version("1.1.0")));
    expect(matches(*r, *parse_version("1.9.9")));
    expect(!(matches(*r, *parse_version("2.0.0"))));
    expect(!(matches(*r, *parse_version("0.9.9"))));

    auto zero = parse_req("~0");   expect(fatal(bool(zero)));
    expect(matches(*zero, *parse_version("0.4.0")));
    expect(!(matches(*zero, *parse_version("1.0.0"))));

    // With the minor written, tilde holds it, as before.
    auto minor = parse_req("~1.2");   expect(fatal(bool(minor)));
    expect(matches(*minor, *parse_version("1.2.9")));
    expect(!(matches(*minor, *parse_version("1.3.0"))));
}};

UTest match_range_and{"VersionReq/MatchRangeAnd", [] {
    auto r = parse_req(">=1.2, <2.0");   expect(fatal(bool(r)));
    expect(matches(*r, *parse_version("1.2.0")));
    expect(matches(*r, *parse_version("1.99.99")));
    expect(!(matches(*r, *parse_version("1.1.99"))));
    expect(!(matches(*r, *parse_version("2.0.0"))));
}};

UTest choose_best{"VersionReq/ChooseBest", [] {
    std::vector<Version> avail = {
        *parse_version("1.0.0"),
        *parse_version("1.2.0"),
        *parse_version("1.2.5"),
        *parse_version("2.0.0"),
    };
    auto r = parse_req("^1.2");   expect(fatal(bool(r)));
    auto pick = choose(*r, avail);
    expect(fatal(bool(pick)));
    expect(eq(avail[*pick].str(), "1.2.5"));
}};

UTest choose_unsatisfiable{"VersionReq/ChooseUnsatisfiable", [] {
    std::vector<Version> avail = { *parse_version("1.0.0"), *parse_version("2.0.0") };
    auto r = parse_req("^3");   expect(fatal(bool(r)));
    expect(!(choose(*r, avail).has_value()));
}};

UTest rejects_garbage{"VersionReq/RejectsGarbage", [] {
    expect(!(parse_req(">=foo").has_value()));
    expect(!(parse_version("not-a-version").has_value()));
}};

// ─── Date-based versions (YYYY.M.D.N), mcpp's own scheme ────────────────
//
// Before the fourth segment existed, parse_version truncated at three and
// every release of a given day compared EQUAL — which silently disabled the
// E0006 index-floor check (index_contract.cppm compares mcpp's own version).

UTest date_version_parses_four_segments{"VersionReq/DateVersionParsesFourSegments", [] {
    auto v = parse_version("2026.7.27.1");
    expect(fatal(bool(v)));
    expect(eq(v->major(), 2026));
    expect(eq(v->minor(), 7));
    expect(eq(v->patch(), 27));
    expect(eq(v->revision(), 1));
}};

UTest date_version_orders_within_the_same_day{"VersionReq/DateVersionOrdersWithinTheSameDay", [] {
    expect(lt(*parse_version("2026.7.27.1"), *parse_version("2026.7.27.2")));
    // Numeric, not lexicographic: the 10th release of a day must outrank the 9th.
    expect(lt(*parse_version("2026.7.27.9"), *parse_version("2026.7.27.10")));
    expect(eq(*parse_version("2026.7.27.3"), *parse_version("2026.7.27.3")));
}};

UTest date_version_outranks_legacy_semver{"VersionReq/DateVersionOutranksLegacySemver", [] {
    expect(lt(*parse_version("0.0.109"), *parse_version("2026.7.27.1")));
}};

UTest three_segment_versions_still_compare_equal_across_written_length{"VersionReq/ThreeSegmentVersionsStillCompareEqualAcrossWrittenLength", [] {
    // `components` must not leak into ordering.
    expect(eq(*parse_version("1.2"), *parse_version("1.2.0")));
    expect(eq(*parse_version("1"), *parse_version("1.0.0")));
    expect(eq(*parse_version("1.2.3"), *parse_version("1.2.3")));
}};

// str() feeds pm/resolver.cppm's resolved-version return value, which becomes
// the lock entry and the xlings wire address — it has to reproduce the
// index's literal version key.
UTest str_keeps_three_segment_normalization_unchanged{"VersionReq/StrKeepsThreeSegmentNormalizationUnchanged", [] {
    expect(eq(parse_version("1.15.2")->str(), "1.15.2"));
    expect(eq(parse_version("0.5")->str(), "0.5.0"));
    expect(eq(parse_version("7")->str(), "7.0.0"));
}};

UTest str_renders_the_fourth_segment_when_written{"VersionReq/StrRendersTheFourthSegmentWhenWritten", [] {
    expect(eq(parse_version("2026.7.27.1")->str(), "2026.7.27.1"));
    // A trailing ".0" is mcpp's formal-release marker and must NOT collapse
    // to three segments — "2026.8.1" is a key that does not exist.
    expect(eq(parse_version("2026.8.1.0")->str(), "2026.8.1.0"));
}};

UTest caret_upper_bound_zeroes_the_fourth_segment{"VersionReq/CaretUpperBoundZeroesTheFourthSegment", [] {
    auto r = parse_req("^2026.7.27.3");   expect(fatal(bool(r)));
    expect(matches(*r, *parse_version("2026.7.27.3")));
    expect(matches(*r, *parse_version("2026.9.1.0")));
    // Upper bound is 2027.0.0.0 — it must not inherit the constraint's ".3".
    expect(!(matches(*r, *parse_version("2027.0.0.0"))));
    expect(!(matches(*r, *parse_version("2027.0.0.2"))));
}};

UTest tilde_upper_bound_zeroes_the_fourth_segment{"VersionReq/TildeUpperBoundZeroesTheFourthSegment", [] {
    auto r = parse_req("~2026.7.27.3");   expect(fatal(bool(r)));
    expect(matches(*r, *parse_version("2026.7.99.0")));
    expect(!(matches(*r, *parse_version("2026.8.0.0"))));
    expect(!(matches(*r, *parse_version("2026.8.0.5"))));
}};

UTest exact_match_distinguishes_same_day_releases{"VersionReq/ExactMatchDistinguishesSameDayReleases", [] {
    auto r = parse_req("=2026.7.27.1");   expect(fatal(bool(r)));
    expect(matches(*r, *parse_version("2026.7.27.1")));
    expect(!(matches(*r, *parse_version("2026.7.27.2"))));
}};

UTest choose_best_among_date_versions{"VersionReq/ChooseBestAmongDateVersions", [] {
    std::vector<Version> avail = {
        *parse_version("2026.7.27.1"),
        *parse_version("2026.7.27.10"),
        *parse_version("2026.7.27.9"),
    };
    auto r = parse_req("*");   expect(fatal(bool(r)));
    auto pick = choose(*r, avail);
    expect(fatal(bool(pick)));
    expect(eq(avail[*pick].str(), "2026.7.27.10"));
}};

// ─── #363: a version is an ORDER; the literal key is the identity ────────
//
// Every case below is a shape the real xim-pkgindex publishes today, not a
// hypothetical: `compat.imgui` ships 1.92.8 alongside 1.92.8-docking (a
// different upstream branch, a different tarball), `jdk-temurin` ships
// 25.0.4+7, `jdk-corretto` ships the five-segment 25.0.4.7.1, and `khistory`
// publishes only `pre-v0.0.5`.

UTest prerelease_sorts_below_its_release{"VersionReq/PrereleaseSortsBelowItsRelease", [] {
    expect(lt(*parse_version("1.92.8-docking"), *parse_version("1.92.8")));
    expect(lt(*parse_version("1.3.3-beta.1"), *parse_version("1.3.3")));
    // ...and above the previous release.
    expect(lt(*parse_version("1.3.2"), *parse_version("1.3.3-beta.1")));
}};

UTest prerelease_identifiers_compare_by_sem_ver_rules{"VersionReq/PrereleaseIdentifiersCompareBySemVerRules", [] {
    // Numeric identifiers compare numerically, not lexicographically.
    expect(lt(*parse_version("1.0.0-rc.9"), *parse_version("1.0.0-rc.10")));
    // Numeric identifiers rank BELOW alphanumeric ones (SemVer §11.4.3).
    expect(lt(*parse_version("1.0.0-1"), *parse_version("1.0.0-alpha")));
    // A longer identifier list outranks its own prefix (§11.4.4).
    expect(lt(*parse_version("1.0.0-alpha"), *parse_version("1.0.0-alpha.1")));
    // Distinct pre-releases are never equal — the property that keeps
    // 1.92.8-docking and 1.92.8-nodocking addressable as two things.
    expect(ne(*parse_version("1.92.8-docking"), *parse_version("1.92.8-legacy")));
}};

UTest ranges_do_not_see_prereleases_unless_asked_for{"VersionReq/RangesDoNotSeePrereleasesUnlessAskedFor", [] {
    // The #363 headline: ^1.92.8 must not silently pick up the docking branch.
    auto caret = parse_req("^1.92.8");   expect(fatal(bool(caret)));
    expect(matches(*caret, *parse_version("1.92.8")));
    expect(!(matches(*caret, *parse_version("1.92.8-docking"))));

    // `*` never reaches a pre-release either.
    auto any = parse_req("*");           expect(fatal(bool(any)));
    expect(!(matches(*any, *parse_version("1.92.8-docking"))));

    // Naming a pre-release at the same numeric tuple opts in.
    auto optIn = parse_req("^1.92.8-a"); expect(fatal(bool(optIn)));
    expect(matches(*optIn, *parse_version("1.92.8-docking")));

    // Exact still addresses it (this is the path `imgui = "1.92.8-docking"`
    // takes after try_merge_semver canonicalises the literal to "=...").
    auto exact = parse_req("=1.92.8-docking"); expect(fatal(bool(exact)));
    expect(matches(*exact, *parse_version("1.92.8-docking")));
    expect(!(matches(*exact, *parse_version("1.92.8"))));
}};

UTest caret_upper_bound_does_not_leak_into_the_next_majors_prerelease{"VersionReq/CaretUpperBoundDoesNotLeakIntoTheNextMajorsPrerelease", [] {
    // 2.0.0-alpha sorts BELOW 2.0.0, so a plain `v < upper` bound admitted it.
    auto r = parse_req("^1.2.3");   expect(fatal(bool(r)));
    expect(!(matches(*r, *parse_version("2.0.0-alpha"))));
    expect(!(matches(*r, *parse_version("2.0.0"))));
    expect(matches(*r, *parse_version("1.9.9")));
}};

UTest five_segment_keys_are_not_truncated{"VersionReq/FiveSegmentKeysAreNotTruncated", [] {
    // jdk-corretto's scheme: <feature>.<interim>.<update>.<build>.<revision>.
    // Truncating at four made 25.0.4.7.1 and 25.0.4.7.2 compare EQUAL.
    expect(lt(*parse_version("25.0.4.7.1"), *parse_version("25.0.4.7.2")));
    expect(lt(*parse_version("21.0.12.8.1"), *parse_version("25.0.4.7.1")));
    expect(eq(parse_version("25.0.4.7.1")->str(), "25.0.4.7.1"));
    // Insignificant trailing zeros still compare equal, at any length.
    expect(eq(*parse_version("1.2"), *parse_version("1.2.0.0.0")));
}};

UTest build_metadata_is_excluded_from_precedence_but_parses{"VersionReq/BuildMetadataIsExcludedFromPrecedenceButParses", [] {
    auto v = parse_version("25.0.4+7");
    expect(fatal(bool(v)));
    expect(eq(*v, *parse_version("25.0.4")));   // SemVer §10
    expect(eq(v->str(), "25.0.4"));
}};

UTest unorderable_keys_are_rejected_rather_than_truncated{"VersionReq/UnorderableKeysAreRejectedRatherThanTruncated", [] {
    // llama.cpp-style build numbers, and the two non-numeric keys the real
    // index publishes. Each must be an ERROR here (→ exact-match-only in the
    // resolver), never a silent parse that collapses onto something else.
    expect(!(parse_version("b10069").has_value()));
    expect(!(parse_version("latest").has_value()));
    expect(!(parse_version("nightly").has_value()));
    expect(!(parse_version("pre-v0.0.5").has_value()));
    // Trailing garbage used to parse as 1.2.3 — and therefore compare EQUAL
    // to it, the same silent merge the extra segments exist to prevent.
    expect(!(parse_version("1.2.3abc").has_value()));
    expect(!(parse_version("1.2.").has_value()));
    expect(!(parse_version("1.2.3-").has_value()));
    expect(!(parse_version("1.2.3+").has_value()));
}};

UTest choose_all_reports_precedence_ties{"VersionReq/ChooseAllReportsPrecedenceTies", [] {
    // 25.0.4 and 25.0.4+7 are two addresses at one precedence. choose_all must
    // hand BOTH back so the resolver can refuse instead of guessing.
    std::vector<Version> avail = {
        *parse_version("25.0.4"),
        *parse_version("25.0.4+7"),
        *parse_version("21.0.12"),
    };
    auto r = parse_req("*");   expect(fatal(bool(r)));
    auto best = choose_all(*r, avail);
    expect(fatal(eq(best.size(), 2u)));
    expect(eq(best[0], 0u));
    expect(eq(best[1], 1u));

    // The ordinary case stays a single answer.
    std::vector<Version> plain = { *parse_version("1.0.0"), *parse_version("1.2.0") };
    expect(eq(choose_all(*r, plain).size(), 1u));
}};

UTest mcpp_own_date_versions_are_unaffected_by_prerelease_support{"VersionReq/McppOwnDateVersionsAreUnaffectedByPrereleaseSupport", [] {
    // E0006 (index floor) compares mcpp's own version through this parser.
    // Adding pre-release/metadata handling must not move any of it.
    expect(lt(*parse_version("2026.8.3.3"), *parse_version("2026.8.6.3")));
    expect(lt(*parse_version("2026.8.1"), *parse_version("2026.8.1.1")));
    expect(eq(*parse_version("2026.8.1.0"), *parse_version("2026.8.1")));
    expect(!(parse_version("2026.8.6.3")->isPrerelease()));
    expect(eq(parse_version("2026.8.6.3")->str(), "2026.8.6.3"));
}};
