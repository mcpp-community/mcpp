
import std;
import mcpplibs.mtest;
import mcpp.version;
import mcpp.version_req;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL: the two modules here answer different questions and are
// tested for that difference. `mcpp.version` is this binary's identity;
// `mcpp.version_req` is the grammar every OTHER package's version is read
// with. Nothing outside this package is involved.

UTest this_binary_declares_a_four_segment_date_version{"Versioning/ThisBinaryDeclaresAFourSegmentDateVersion", [] {
    // The same string lives in the root mcpp.toml, and
    // .github/tools/check_version_pins.sh keeps the two equal. This states the
    // SHAPE, which that checker cannot: a version that lost a segment would
    // still match the manifest and still be wrong.
    const std::string v{mcpp::MCPP_VERSION};
    expect(!v.empty());
    expect(eq(std::ranges::count(v, '.'), 3)) << "expected YYYY.M.D.N, got " << v;
}};

UTest the_fourth_segment_survives_a_parse_and_print{"Versioning/TheFourthSegmentSurvivesAParseAndPrint", [] {
    // Round-tripping is load-bearing: the resolver rebuilds dependency version
    // strings from this, and they flow into the lock file and the xlings wire
    // address. A `.0` collapsing to three segments would rename a package.
    auto v = mcpp::version_req::parse_version("2026.8.29.1");
    if (auto&& failed = expect(fatal(v.has_value())); !failed) failed << v.error();
    expect(eq(v->str(), "2026.8.29.1"));

    auto z = mcpp::version_req::parse_version("2026.8.29.0");
    if (auto&& failed = expect(fatal(z.has_value())); !failed) failed << z.error();
    expect(eq(z->str(), "2026.8.29.0"));
}};

UTest the_date_scheme_sorts_above_the_old_three_segment_one{"Versioning/TheDateSchemeSortsAboveTheOldThreeSegmentOne", [] {
    // `0.0.109` < `2026.7.27.1`: the first segment goes from 0 to 2026 and
    // never comes back. Stated here because the ordering is what stops a
    // released mcpp from resolving backwards into the pre-date scheme.
    auto older = mcpp::version_req::parse_version("0.0.109");
    auto newer = mcpp::version_req::parse_version("2026.7.27.1");
    expect(fatal(older.has_value()));
    expect(fatal(newer.has_value()));
    expect(*older < *newer);
}};
