
import std;
import mcpplibs.mtest;
import mcpp.pm.compat.registry;
import mcpp.version;

using namespace mcpplibs::mtest;

// THE DEPRECATION REGISTRY (mcpp#797, docs/51 §Deprecation). Every reading a
// user can write and that is to be removed is a row; the warning, `mcpp why
// compat`, docs/51 and the removal itself are derived from it. These tests
// are the part of that derivation the compiler can hold the code to.

using mcpp::pm::compat::calver_month;
using mcpp::pm::compat::kDeprecations;

// THE FORCING FUNCTION. Once mcpp reaches a row's floor, the reading must be
// gone from the code, not merely past its date: this fails the build of the
// release that would otherwise ship it, naming the row.
UTest no_reading_outlives_its_floor{"CompatRegistry/NoReadingOutlivesItsFloor", [] {
    for (auto const& d : kDeprecations)
        expect(!mcpp::pm::compat::past_floor(d, mcpp::MCPP_VERSION))
            << "mcpp " << mcpp::MCPP_VERSION << " is at or past the floor " << d.floor
            << " of COMPAT(" << d.id << "): remove the reading and its row";
}};

// docs/51: at least two whole calendar months between the release that
// deprecates a reading and the floor from which it is removed.
UTest every_floor_leaves_two_whole_months{"CompatRegistry/EveryFloorLeavesTwoWholeMonths", [] {
    for (auto const& d : kDeprecations) {
        auto since = calver_month(d.since);
        auto floor = calver_month(d.floor);
        expect(fatal(since && floor)) << d.id;
        const int months = (floor->first - since->first) * 12 + (floor->second - since->second);
        // The month of `since` itself is not whole: two more must follow it.
        expect(ge(months, 3)) << d.id << ": " << d.since << " -> " << d.floor;
    }
}};

UTest rows_are_complete_and_unique{"CompatRegistry/RowsAreCompleteAndUnique", [] {
    std::set<std::string_view> ids;
    for (auto const& d : kDeprecations) {
        expect(bool(ids.insert(d.id).second)) << "duplicate id " << d.id;
        expect(!d.what.empty()) << d.id;
        expect(!d.canonical.empty()) << d.id;
        expect(!d.domain.empty()) << d.id;
        expect(d.docs.starts_with("docs/")) << d.id;
        expect(ne(mcpp::pm::compat::find_deprecation(d.id), nullptr));
    }
    expect(eq(mcpp::pm::compat::find_deprecation("no-such-reading"), nullptr));
}};

UTest the_clause_names_both_versions{"CompatRegistry/TheClauseNamesBothVersions", [] {
    auto const* d = mcpp::pm::compat::find_deprecation("workspace-position");
    expect(fatal(ne(d, nullptr)));
    expect(eq(mcpp::pm::compat::deprecation_clause(*d), "deprecated since mcpp 2026.10.10.1 and removed from mcpp 2027.1"));
}};

UTest calver_month_orders_versions_and_floors{"CompatRegistry/CalverMonthOrdersVersionsAndFloors", [] {
    expect(eq(calver_month("2026.10.11.1"), (std::pair{2026, 10})));
    expect(eq(calver_month("2027.1"), (std::pair{2027, 1})));
    expect(!(calver_month("1.0.0-rc").has_value()));
    mcpp::pm::compat::Deprecation d{"x", "2026.10.1.1", "2027.1", "w", "c", "d", "docs/x"};
    expect(!mcpp::pm::compat::past_floor(d, "2026.12.31.9"));
    expect(bool(mcpp::pm::compat::past_floor(d, "2027.1.2.1")));
    expect(bool(mcpp::pm::compat::past_floor(d, "2028.3.1.1")));
}};

UTest uses_are_recorded_once_and_keep_the_warning{"CompatRegistry/UsesAreRecordedOnceAndKeepTheWarning", [] {
    mcpp::pm::compat::record_use("workspace-position", "/w/mcpp.toml", "[toolchain]", false);
    mcpp::pm::compat::record_use("workspace-position", "/w/mcpp.toml", "[toolchain]", true);
    std::size_t n = 0;
    for (auto const& u : mcpp::pm::compat::recorded_uses())
        if (u.where == "/w/mcpp.toml" && u.detail == "[toolchain]") {
            ++n;
            expect(bool(u.warned));
        }
    expect(eq(n, 1u));
}};
