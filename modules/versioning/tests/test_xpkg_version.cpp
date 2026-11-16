
import std;
import mcpplibs.mtest;
import mcpp.xpkg_version;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL: the grammar xlings resolves an `[xlings]` address with,
// stated on its own. The engine-level consequence (which payload directory
// `mcpp::xpkg_dir` answers) is in tests/unit/test_freestanding.cpp.

namespace xv = mcpp::xpkg_version;

namespace {
std::string best(std::initializer_list<const char*> keys, std::string_view req) {
    std::vector<std::string> v(keys.begin(), keys.end());
    return xv::select_best(v, req).value_or("");
}
}

UTest four_segment_keys_are_ordinary_versions{"XpkgVersion/FourSegmentKeysAreOrdinaryVersions", [] {
    auto v = xv::parse("1.7.0.1");
    expect(fatal(v.has_value()));
    expect(eq(v->components, 4));
    expect(gt(xv::compare_keys("1.7.0.1", "1.7.0"), 0));
    expect(gt(xv::compare_keys("0.0.100", "0.0.11"), 0));
}};

UTest a_bare_version_of_one_or_two_segments_is_a_prefix_range{"XpkgVersion/ABareVersionOfOneOrTwoSegmentsIsAPrefixRange", [] {
    expect(eq(best({"1.7.0.1", "1.8.0"}, "1.7"), "1.7.0.1"));
    expect(eq(best({"1.2.0", "1.2.5", "1.3.0"}, "1.2"), "1.2.5"));
    expect(eq(best({"15.1.0", "15.2.0", "16.1.0"}, "15"), "15.2.0"));
}};

UTest a_bare_version_of_three_or_more_segments_is_written_prefix_equality{"XpkgVersion/ABareVersionOfThreeOrMoreSegmentsIsWrittenPrefixEquality", [] {
    expect(eq(best({"12.9.1.4", "12.9.10"}, "12.9.1"), "12.9.1.4"));
    expect(eq(best({"1.2.0", "1.2.5"}, "1.2.0"), "1.2.0"));
    expect(eq(best({"1.8.13", "1.9.0"}, "1.8.12"), ""));
    expect(eq(best({"2.15.0.1", "2.15.0.2"}, "2.15.0.1"), "2.15.0.1"));
}};

UTest operators_and_conjunctions_compare_the_whole_version{"XpkgVersion/OperatorsAndConjunctionsCompareTheWholeVersion", [] {
    expect(eq(best({"8.0.0", "8.5.0", "8.7.1"}, ">=8.5.0"), "8.7.1"));
    expect(eq(best({"8.0.0", "8.5.0", "8.7.1"}, ">=8.0.0 <8.7.0"), "8.5.0"));
    expect(eq(best({"2.15.0", "2.15.0.1"}, ">=2.15.0.1"), "2.15.0.1"));
    expect(eq(best({"1.2.3", "1.9.0", "2.0.0"}, "^1.2.3"), "1.9.0"));
    expect(eq(best({"1.2.3", "1.2.9", "1.3.0"}, "~1.2.3"), "1.2.9"));
    expect(eq(best({"1.2.3", "1.2.9", "1.3.0"}, "1.2.*"), "1.2.9"));
}};

UTest a_prerelease_is_exact_and_ranks_below_its_release{"XpkgVersion/APrereleaseIsExactAndRanksBelowItsRelease", [] {
    expect(eq(best({"1.0.0-rc1", "1.0.0"}, "1.0.0-rc1"), "1.0.0-rc1"));
    expect(lt(xv::compare_keys("1.0.0-rc1", "1.0.0"), 0));
}};

UTest names_never_win_and_build_metadata_is_ignored{"XpkgVersion/NamesNeverWinAndBuildMetadataIsIgnored", [] {
    expect(eq(best({"latest", "nightly", "1.0.0"}, ">=0.1"), "1.0.0"));
    expect(!(xv::parse("nightly").has_value()));
    expect(gt(xv::compare_keys("1.0.0", "nightly"), 0));
    expect(eq(best({"25.0.4+7"}, "25.0.4"), "25.0.4+7"));
}};

// THE SHARED STATEMENT. xlings publishes which version a request selects as
// data (tests/data/semver-vectors.tsv in its repository), and this package
// vendors the file at the xlings version mcpp pins. Every vector is run
// through `select_installed`, the function `mcpp::xpkg_dir` answers with.
// A vector whose `active` column names a version is skipped: the active
// version is state xlings keeps, and mcpp selects among installed payload
// directories, none of which is active.
namespace {
std::filesystem::path vectors_file() {
    namespace fs = std::filesystem;
    for (auto dir = fs::current_path();; dir = dir.parent_path()) {
        for (auto rel : {"tests/data/semver-vectors.tsv",
                         "modules/versioning/tests/data/semver-vectors.tsv"}) {
            std::error_code ec;
            if (fs::is_regular_file(dir / rel, ec)) return dir / rel;
        }
        if (dir == dir.parent_path()) return {};
    }
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    for (std::size_t p = 0;;) {
        const auto e = s.find(sep, p);
        out.emplace_back(s.substr(p, e == std::string_view::npos ? e : e - p));
        if (e == std::string_view::npos) return out;
        p = e + 1;
    }
}
}

UTest the_xlings_conformance_vectors_select_the_same_version{"XpkgVersion/TheXlingsConformanceVectorsSelectTheSameVersion", [] {
    const auto file = vectors_file();
    expect(fatal(!file.empty())) << "semver-vectors.tsv not found above "
                               << std::filesystem::current_path();
    std::ifstream in(file);
    std::string line;
    int lineNo = 0, checked = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.empty() || line.front() == '#') continue;
        const auto col = split(line, '\t');
        expect(fatal(eq(col.size(), 4u))) << file << ":" << lineNo;
        if (col[2] != "-") continue;
        const auto available = split(col[1], ',');
        const auto request = col[0] == "-" ? std::string{} : col[0];
        if (auto&& failed = expect(eq(xv::select_installed(available, request).value_or("none"), col[3])); !failed) failed << file << ":" << lineNo << ": request '" << col[0] << "' among " << col[1];
        ++checked;
    }
    expect(gt(checked, 0));
}};
