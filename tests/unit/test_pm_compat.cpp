#include <gtest/gtest.h>

import std;
import mcpp.pm.compat;
import mcpp.pm.dependency_selector;

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

TEST(DependencySelector, DottedSelectorBuildsOmittedMcpplibsPriorityCandidates) {
    auto selector = mcpp::pm::resolve_dependency_selector(
        "imgui.backend.glfw_opengl3",
        mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);

    EXPECT_EQ(selector.stableMapKey, "imgui.backend.glfw_opengl3");
    ASSERT_EQ(selector.candidates.size(), 2u);
    EXPECT_EQ(selector.candidates[0].namespace_, "mcpplibs.imgui.backend");
    EXPECT_EQ(selector.candidates[0].shortName, "glfw_opengl3");
    EXPECT_EQ(selector.candidates[1].namespace_, "imgui.backend");
    EXPECT_EQ(selector.candidates[1].shortName, "glfw_opengl3");
}

TEST(DependencySelector, ExplicitMcpplibsPrefixDoesNotAddPeerFallback) {
    auto selector = mcpp::pm::resolve_dependency_selector(
        "mcpplibs.capi.lua",
        mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);

    EXPECT_EQ(selector.stableMapKey, "mcpplibs.capi.lua");
    ASSERT_EQ(selector.candidates.size(), 1u);
    EXPECT_EQ(selector.candidates[0].namespace_, "mcpplibs.capi");
    EXPECT_EQ(selector.candidates[0].shortName, "lua");
}

TEST(DependencySelector, ExplicitRootSelectorHasOnlyThatRoot) {
    auto selector = mcpp::pm::make_direct_dependency_selector(
        "compat", "gtest", "compat.gtest");

    EXPECT_EQ(selector.stableMapKey, "compat.gtest");
    ASSERT_EQ(selector.candidates.size(), 1u);
    EXPECT_EQ(selector.candidates[0].namespace_, "compat");
    EXPECT_EQ(selector.candidates[0].shortName, "gtest");
}
