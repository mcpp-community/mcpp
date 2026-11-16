// merge_xpkg_versions_desc — the union view over a descriptor's per-OS
// version tables (#487). Ordering must follow SemVer on the PARSED value while
// display keeps the ORIGINAL key byte-for-byte (#363's lesson: an arbitrary
// index key cannot be reproduced from its parsed form).


import std;
import mcpplibs.mtest;
import mcpp.manifest;

using namespace mcpplibs::mtest;

namespace {

UTest orders_semver_descending_across_platforms{"XpkgVersionsMerge/OrdersSemverDescendingAcrossPlatforms", [] {
    std::vector<std::vector<std::string>> perPlatform = {
        {"1.0.0"},
        {"0.9.0", "1.2.0"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    expect(fatal(eq(merged.size(), 3u)));
    expect(eq(merged[0], "1.2.0"));
    expect(eq(merged[1], "1.0.0"));
    expect(eq(merged[2], "0.9.0"));
}};

UTest deduplicates_keys_shared_across_platforms{"XpkgVersionsMerge/DeduplicatesKeysSharedAcrossPlatforms", [] {
    std::vector<std::vector<std::string>> perPlatform = {
        {"1.0.0", "0.9.0"},
        {"1.0.0", "0.8.0"},
        {"1.0.0"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    expect(fatal(eq(merged.size(), 3u)));
    expect(eq(merged[0], "1.0.0"));
    expect(eq(merged[1], "0.9.0"));
    expect(eq(merged[2], "0.8.0"));
}};

UTest release_outranks_prerelease_of_same_numbers{"XpkgVersionsMerge/ReleaseOutranksPrereleaseOfSameNumbers", [] {
    std::vector<std::vector<std::string>> perPlatform = {
        {"1.0.0-rc.1", "1.0.0"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    expect(fatal(eq(merged.size(), 2u)));
    expect(eq(merged[0], "1.0.0"));
    expect(eq(merged[1], "1.0.0-rc.1"));
}};

UTest prerelease_numeric_identifiers_compare_numerically{"XpkgVersionsMerge/PrereleaseNumericIdentifiersCompareNumerically", [] {
    std::vector<std::vector<std::string>> perPlatform = {
        {"1.0.0-rc.2", "1.0.0-rc.10"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    expect(fatal(eq(merged.size(), 2u)));
    expect(eq(merged[0], "1.0.0-rc.10"));
    expect(eq(merged[1], "1.0.0-rc.2"));
}};

UTest fourth_segment_compares_numerically{"XpkgVersionsMerge/FourthSegmentComparesNumerically", [] {
    std::vector<std::vector<std::string>> perPlatform = {
        {"1.2.3.10", "1.2.3.9"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    expect(fatal(eq(merged.size(), 2u)));
    expect(eq(merged[0], "1.2.3.10"));
    expect(eq(merged[1], "1.2.3.9"));
}};

UTest keeps_original_key_for_suffixed_builds{"XpkgVersionsMerge/KeepsOriginalKeyForSuffixedBuilds", [] {
    // compat.glad publishes keys like "0.0.0-651a425": the suffix rides along
    // as pre-release identifiers for ordering, but what reaches the screen is
    // the key as written.
    std::vector<std::vector<std::string>> perPlatform = {
        {"0.0.0-651a425", "0.0.1"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    expect(fatal(eq(merged.size(), 2u)));
    expect(eq(merged[0], "0.0.1"));
    expect(eq(merged[1], "0.0.0-651a425"));
}};

UTest unparsable_keys_sort_last_by_name_and_keep_their_text{"XpkgVersionsMerge/UnparsableKeysSortLastByNameAndKeepTheirText", [] {
    std::vector<std::vector<std::string>> perPlatform = {
        {"unknown", "1.0.0", "beta-thing"},
    };
    auto merged = mcpp::manifest::merge_xpkg_versions_desc(perPlatform);
    expect(fatal(eq(merged.size(), 3u)));
    expect(eq(merged[0], "1.0.0"));
    expect(eq(merged[1], "beta-thing"));
    expect(eq(merged[2], "unknown"));
}};

UTest empty_inputs_yield_empty_output{"XpkgVersionsMerge/EmptyInputsYieldEmptyOutput", [] {
    expect(mcpp::manifest::merge_xpkg_versions_desc({}).empty());
    expect(mcpp::manifest::merge_xpkg_versions_desc({{}, {}}).empty());
}};

// A truncated line must say it is truncated. Without the marker the default
// three-version view reads as the complete set, which is the one wrong
// conclusion the display can produce -- and both `mcpp search` and the
// `mcpp add` suggestion render through this function, so one assertion
// covers the contract for both.
UTest truncation_is_marked_and_an_exact_fit_is_not{"JoinVersionsDesc/TruncationIsMarkedAndAnExactFitIsNot", [] {
    const std::vector<std::string> six = {"0.0.6", "0.0.5", "0.0.4",
                                          "0.0.3", "0.0.2", "0.0.1"};
    expect(eq(mcpp::manifest::join_versions_desc(six), "0.0.6, 0.0.5, 0.0.4, ..."));
    expect(eq(mcpp::manifest::join_versions_desc(six, six.size()), "0.0.6, 0.0.5, 0.0.4, 0.0.3, 0.0.2, 0.0.1"));

    // Exactly `keep` entries is not a truncation.
    const std::vector<std::string> three = {"2.0.0", "1.0.0", "0.9.0"};
    expect(eq(mcpp::manifest::join_versions_desc(three), "2.0.0, 1.0.0, 0.9.0"));

    expect(eq(mcpp::manifest::join_versions_desc({}), ""));
    expect(eq(mcpp::manifest::join_versions_desc({"1.0.0"}), "1.0.0"));
}};

} // namespace
