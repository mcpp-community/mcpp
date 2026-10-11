#include <gtest/gtest.h>

import std;
import mcpp.pm.compat.registry;
import mcpp.version;

// THE DEPRECATION REGISTRY (mcpp#797, docs/51 §Deprecation). Every reading a
// user can write and that is to be removed is a row; the warning, `mcpp why
// compat`, docs/51 and the removal itself are derived from it. These tests
// are the part of that derivation the compiler can hold the code to.

using mcpp::pm::compat::calver_month;
using mcpp::pm::compat::kDeprecations;

// THE FORCING FUNCTION. Once mcpp reaches a row's floor, the reading must be
// gone from the code, not merely past its date: this fails the build of the
// release that would otherwise ship it, naming the row.
TEST(CompatRegistry, NoReadingOutlivesItsFloor) {
    for (auto const& d : kDeprecations)
        EXPECT_FALSE(mcpp::pm::compat::past_floor(d, mcpp::MCPP_VERSION))
            << "mcpp " << mcpp::MCPP_VERSION << " is at or past the floor " << d.floor
            << " of COMPAT(" << d.id << "): remove the reading and its row";
}

// docs/51: at least two whole calendar months between the release that
// deprecates a reading and the floor from which it is removed.
TEST(CompatRegistry, EveryFloorLeavesTwoWholeMonths) {
    for (auto const& d : kDeprecations) {
        auto since = calver_month(d.since);
        auto floor = calver_month(d.floor);
        ASSERT_TRUE(since && floor) << d.id;
        const int months = (floor->first - since->first) * 12 + (floor->second - since->second);
        // The month of `since` itself is not whole: two more must follow it.
        EXPECT_GE(months, 3) << d.id << ": " << d.since << " -> " << d.floor;
    }
}

TEST(CompatRegistry, RowsAreCompleteAndUnique) {
    std::set<std::string_view> ids;
    for (auto const& d : kDeprecations) {
        EXPECT_TRUE(ids.insert(d.id).second) << "duplicate id " << d.id;
        EXPECT_FALSE(d.what.empty()) << d.id;
        EXPECT_FALSE(d.canonical.empty()) << d.id;
        EXPECT_FALSE(d.domain.empty()) << d.id;
        EXPECT_TRUE(d.docs.starts_with("docs/")) << d.id;
        EXPECT_NE(mcpp::pm::compat::find_deprecation(d.id), nullptr);
    }
    EXPECT_EQ(mcpp::pm::compat::find_deprecation("no-such-reading"), nullptr);
}

TEST(CompatRegistry, TheClauseNamesBothVersions) {
    auto const* d = mcpp::pm::compat::find_deprecation("workspace-position");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(mcpp::pm::compat::deprecation_clause(*d),
              "deprecated since mcpp 2026.10.10.1 and removed from mcpp 2027.1");
}

TEST(CompatRegistry, CalverMonthOrdersVersionsAndFloors) {
    EXPECT_EQ(calver_month("2026.10.11.1"), (std::pair{2026, 10}));
    EXPECT_EQ(calver_month("2027.1"), (std::pair{2027, 1}));
    EXPECT_FALSE(calver_month("1.0.0-rc").has_value());
    mcpp::pm::compat::Deprecation d{"x", "2026.10.1.1", "2027.1", "w", "c", "d", "docs/x"};
    EXPECT_FALSE(mcpp::pm::compat::past_floor(d, "2026.12.31.9"));
    EXPECT_TRUE(mcpp::pm::compat::past_floor(d, "2027.1.2.1"));
    EXPECT_TRUE(mcpp::pm::compat::past_floor(d, "2028.3.1.1"));
}

TEST(CompatRegistry, UsesAreRecordedOnceAndKeepTheWarning) {
    mcpp::pm::compat::record_use("workspace-position", "/w/mcpp.toml", "[toolchain]", false);
    mcpp::pm::compat::record_use("workspace-position", "/w/mcpp.toml", "[toolchain]", true);
    std::size_t n = 0;
    for (auto const& u : mcpp::pm::compat::recorded_uses())
        if (u.where == "/w/mcpp.toml" && u.detail == "[toolchain]") {
            ++n;
            EXPECT_TRUE(u.warned);
        }
    EXPECT_EQ(n, 1u);
}
