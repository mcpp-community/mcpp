#include <gtest/gtest.h>

import std;

// #610 / #630 item 5 — `[target.<triple>]` has a scalar/array parser (a
// sequence of `body.find("<key>")` blocks) and, further down in the same
// function, an unknown-key sweep that reports anything not named in
// `kKnownTargetScalars` / `kKnownTargetArrays`. The two lists are hand-written
// text living next to hand-written parse calls, and they drifted once
// already: `min_api_level` was added to the parser without being added to the
// sweep's lists, so a manifest that spelled it correctly was told the key was
// unsupported, and `--strict` turned that into a build failure.
//
// A test that hand-writes its own "the known keys are: ..." list would only
// re-encode the same assumption the sweep's list encodes, and would drift
// with it. Instead this test reads `modules/manifest/src/toml.cppm` as text
// and derives the set of parsed keys from the same `body.find("<key>")` calls
// the parser executes, then checks it against the sweep's lists in both
// directions:
//
//   - every parsed non-table key is named in the matching known-list, so a
//     sixth key added to the parser without a matching list entry fails
//     here instead of shipping silently;
//   - every entry in a known-list has a matching parse site, so a stale or
//     misspelled list entry (one that names nothing the parser reads) is
//     caught too.
//
// The denominator — the set of `body.find(...)` calls — comes from the
// source tree, not from a list re-typed in this file, which is what makes
// the test unable to go stale the same way the code did.

namespace {

std::string read_file(const std::filesystem::path& p) {
    std::ifstream input(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), {});
}

std::filesystem::path repo_root() {
    // tests/unit/test_target_scalar_keys.cpp -> tests/unit -> tests -> repo root
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

// The region of toml.cppm that both the scalar/array parser and the
// unknown-key sweep operate on: from the start of the `[target.<triple>]`
// loop body up to the assignment that closes it, `m.targetOverrides[...] =
// std::move(e);`. Bounding the scan to this region keeps it from picking up
// unrelated `body.find(...)` calls in the conditional-config channel that
// follows in the same function (`abi`, `runtime`, `build`, `xlings`, ...),
// which read the same variable name for a different table.
std::string target_entry_region(const std::string& source) {
    auto begin = source.find("for (auto& [triple, val] : *tt) {");
    auto end = source.find("m.targetOverrides[canon_triple(triple)] = std::move(e);");
    if (begin == std::string::npos || end == std::string::npos || end < begin) return {};
    return source.substr(begin, end - begin);
}

enum class Kind { Scalar, Array, Table };

// Classifies a `body.find("key")` call by which type-check appears first
// after it: the parser always tests the found value's type before reading
// it, and does so with `is_table()`, `is_array()`, `is_string()` or
// `is_int()` — `min_api_level` uses the last of these, everything else
// currently uses one of the first three.
Kind classify(const std::string& region, std::size_t keyPos, std::size_t nextKeyPos) {
    std::size_t window_end = std::min(nextKeyPos, region.size());
    auto first_of = [&](std::string_view needle) {
        auto p = region.find(needle, keyPos);
        return (p == std::string::npos || p >= window_end) ? std::string::npos : p;
    };
    std::size_t table = first_of("is_table()");
    std::size_t array = first_of("is_array()");
    std::size_t string_ = first_of("is_string()");
    std::size_t int_ = first_of("is_int()");
    std::size_t scalar = std::min(string_, int_);
    if (table != std::string::npos && table < array && table < scalar) return Kind::Table;
    if (array != std::string::npos && array < scalar) return Kind::Array;
    return Kind::Scalar;
}

struct ParsedKeys {
    std::vector<std::string> scalars;
    std::vector<std::string> arrays;
};

ParsedKeys parsed_target_keys(const std::string& region) {
    ParsedKeys out;
    const std::string needle = "body.find(\"";
    std::size_t pos = 0;
    std::vector<std::pair<std::size_t, std::string>> hits;
    while ((pos = region.find(needle, pos)) != std::string::npos) {
        std::size_t nameStart = pos + needle.size();
        std::size_t nameEnd = region.find('"', nameStart);
        hits.emplace_back(pos, region.substr(nameStart, nameEnd - nameStart));
        pos = nameEnd;
    }
    for (std::size_t i = 0; i < hits.size(); ++i) {
        std::size_t nextPos = (i + 1 < hits.size()) ? hits[i + 1].first : region.size();
        Kind k = classify(region, hits[i].first, nextPos);
        const std::string& key = hits[i].second;
        switch (k) {
            case Kind::Scalar: out.scalars.push_back(key); break;
            case Kind::Array:  out.arrays.push_back(key); break;
            case Kind::Table:  break;  // sub-tables are exempt from the sweep
        }
    }
    return out;
}

// Pulls the string literals out of `kKnownTargetScalars[] = { "a", "b", };`
// (or the analogous `kKnownTargetArrays`), the sweep's own known-key lists.
std::vector<std::string> known_list(const std::string& region, std::string_view arrayName) {
    std::vector<std::string> out;
    auto declPos = region.find(arrayName);
    if (declPos == std::string::npos) return out;
    auto braceStart = region.find('{', declPos);
    auto braceEnd = region.find('}', braceStart);
    std::string body = region.substr(braceStart + 1, braceEnd - braceStart - 1);
    std::size_t pos = 0;
    while ((pos = body.find('"', pos)) != std::string::npos) {
        auto end = body.find('"', pos + 1);
        out.push_back(body.substr(pos + 1, end - pos - 1));
        pos = end + 1;
    }
    return out;
}

}  // namespace

TEST(TargetScalarKeys, EveryParsedNonTableKeyIsKnownToTheSweep) {
    auto source = read_file(repo_root() / "modules" / "manifest" / "src" / "toml.cppm");
    ASSERT_FALSE(source.empty()) << "could not read toml.cppm";
    auto region = target_entry_region(source);
    ASSERT_FALSE(region.empty()) << "could not locate the [target.<triple>] entry parser";

    auto parsed = parsed_target_keys(region);
    auto knownScalars = known_list(region, "kKnownTargetScalars");
    auto knownArrays = known_list(region, "kKnownTargetArrays");
    ASSERT_FALSE(knownScalars.empty());
    ASSERT_FALSE(knownArrays.empty());

    // Direction 1 (the positive claim, and the failure mode #610 produced):
    // every key the parser reads as a scalar is in the sweep's scalar list,
    // and likewise for arrays. `min_api_level` is the case that used to fail
    // this.
    for (auto const& key : parsed.scalars)
        EXPECT_NE(std::ranges::find(knownScalars, key), knownScalars.end())
            << "'" << key << "' is parsed as a scalar but missing from "
            << "kKnownTargetScalars, so the sweep would report it as unsupported";
    for (auto const& key : parsed.arrays)
        EXPECT_NE(std::ranges::find(knownArrays, key), knownArrays.end())
            << "'" << key << "' is parsed as an array but missing from "
            << "kKnownTargetArrays";

    // Direction 2 (the negative claim): every entry in a known-list actually
    // names something the parser reads. A stale list entry would pass every
    // manifest silently and give no test failure otherwise.
    for (auto const& key : knownScalars)
        EXPECT_NE(std::ranges::find(parsed.scalars, key), parsed.scalars.end())
            << "kKnownTargetScalars names '" << key << "', which has no "
            << "body.find(...) parse site in the [target.<triple>] entry parser";
    for (auto const& key : knownArrays)
        EXPECT_NE(std::ranges::find(parsed.arrays, key), parsed.arrays.end())
            << "kKnownTargetArrays names '" << key << "', which has no "
            << "body.find(...) parse site";
}

TEST(TargetScalarKeys, MinApiLevelIsParsedAndKnown) {
    // A direct, non-derived check on the specific regression: `min_api_level`
    // must be both a parse site and a known scalar key. If this test passes
    // while the source-scan test above also passes, the fix is coherent; if
    // this one passes and the scan test fails, a *different* key drifted.
    auto source = read_file(repo_root() / "modules" / "manifest" / "src" / "toml.cppm");
    auto region = target_entry_region(source);
    ASSERT_FALSE(region.empty());
    EXPECT_NE(region.find("body.find(\"min_api_level\")"), std::string::npos);
    auto knownScalars = known_list(region, "kKnownTargetScalars");
    EXPECT_NE(std::ranges::find(knownScalars, std::string("min_api_level")), knownScalars.end());
}
