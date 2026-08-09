#include <gtest/gtest.h>

import std;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;

// The one-release exit ramp for namespace-omitted selectors. Kept in its
// own file so removing the window in 2026.9 is one deletion, not an
// archaeology exercise across the resolver tests.

TEST(PmBareNameWindow, LegacyRungsExistOnlyForAnOmittedNamespace) {
    // Eligible: the author omitted the namespace, so `mcpplibs` was supplied.
    auto omitted = mcpp::pm::legacy_bare_candidates(
        mcpp::pm::DependencyCoordinate{
            .namespace_ = std::string(mcpp::pm::kDefaultNamespace),
            .shortName  = "gtest",
        });
    ASSERT_EQ(omitted.size(), 2u);
    EXPECT_EQ(omitted[0].namespace_, mcpp::pm::kCompatNamespace);
    EXPECT_EQ(omitted[0].shortName, "gtest");
    // The namespace-less discovery rung, for upstream descriptors that declare
    // no namespace at all. Callers must still reject a descriptor that DOES
    // declare one, or this is the cross-namespace wildcard #278 removed.
    EXPECT_TRUE(omitted[1].namespace_.empty());
    EXPECT_EQ(omitted[1].shortName, "gtest");

    // Not eligible: every one of these STATES an identity. A stated identity
    // that misses is a miss — the window exists for spellings that predate the
    // rule, not for ones that break it.
    for (std::string_view ns : { "compat", "acme", "mcpplibs.capi" }) {
        EXPECT_TRUE(mcpp::pm::legacy_bare_candidates(
            mcpp::pm::DependencyCoordinate{
                .namespace_ = std::string(ns),
                .shortName  = "gtest",
            }).empty()) << ns;
    }
}
