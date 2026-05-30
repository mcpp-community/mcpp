#include <gtest/gtest.h>

import std;
import mcpp.pm.compat;

TEST(PmCompat, InstallDirCandidatesIncludeNestedNamespaceFallback) {
    auto candidates = mcpp::pm::compat::install_dir_candidates(
        "mcpplibs", "capi.lua", "mcpplibs");

    EXPECT_NE(
        std::find(candidates.begin(), candidates.end(),
                  "mcpplibs.capi-x-mcpplibs.capi.lua"),
        candidates.end());
}

TEST(PmCompat, NormalizeNestedNamespacePreservesQualifiedName) {
    std::string ns = "mcpplibs";
    std::string shortName = "capi.lua";

    mcpp::pm::compat::normalize_nested_namespace(ns, shortName,
                                                  /*legacyDottedKey=*/true);

    EXPECT_EQ(ns, "mcpplibs.capi");
    EXPECT_EQ(shortName, "lua");
    EXPECT_EQ(mcpp::pm::compat::qualified_name(ns, shortName),
              "mcpplibs.capi.lua");
}

TEST(PmCompat, SplitLegacyDependencyKeyMarksDottedKeyAsCompat) {
    auto key = mcpp::pm::compat::split_legacy_dependency_key(
        "mcpplibs.capi.lua");

    EXPECT_EQ(key.namespace_, "mcpplibs");
    EXPECT_EQ(key.shortName, "capi.lua");
    EXPECT_TRUE(key.legacyDottedKey);
}

TEST(PmCompat, NormalizeNestedNamespaceSkipsCanonicalNamespacedDeps) {
    std::string ns = "mcpplibs.capi";
    std::string shortName = "lua.extra";

    mcpp::pm::compat::normalize_nested_namespace(ns, shortName,
                                                  /*legacyDottedKey=*/false);

    EXPECT_EQ(ns, "mcpplibs.capi");
    EXPECT_EQ(shortName, "lua.extra");
}
