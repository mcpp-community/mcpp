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

TEST(DependencySelector, DottedSelectorIsOneExactNamespace) {
    auto selector = mcpp::pm::resolve_dependency_selector(
        "imgui.backend.glfw_opengl3");

    EXPECT_EQ(selector.stableMapKey, "imgui.backend.glfw_opengl3");
    ASSERT_EQ(selector.candidates.size(), 1u);
    EXPECT_EQ(selector.candidates[0].namespace_, "imgui.backend");
    EXPECT_EQ(selector.candidates[0].shortName, "glfw_opengl3");
}

TEST(DependencySelector, BareSelectorUsesOnlyDefaultNamespace) {
    auto selector = mcpp::pm::resolve_dependency_selector("imgui");

    EXPECT_EQ(selector.stableMapKey, "imgui");
    ASSERT_EQ(selector.candidates.size(), 1u);
    EXPECT_EQ(selector.candidates[0].namespace_, "mcpplibs");
    EXPECT_EQ(selector.candidates[0].shortName, "imgui");
}

TEST(DependencySelector, ExplicitMcpplibsPrefixDoesNotAddPeerFallback) {
    auto selector = mcpp::pm::resolve_dependency_selector(
        "mcpplibs.capi.lua");

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

TEST(DependencySelector, SharedParserNormalizesBareAndDottedSelectors) {
    auto bare = mcpp::pm::parse_package_selector("lua");
    ASSERT_TRUE(bare.has_value());
    EXPECT_FALSE(bare->namespace_.has_value());
    EXPECT_EQ(bare->name, "lua");
    EXPECT_EQ(bare->spelling, "lua");

    auto bareCoord = mcpp::pm::normalize_package_selector(*bare);
    EXPECT_EQ(bareCoord.namespace_, "mcpplibs");
    EXPECT_EQ(bareCoord.shortName, "lua");
    EXPECT_EQ(mcpp::pm::format_package_selector(bareCoord), "lua");

    auto nested = mcpp::pm::parse_package_selector("mcpplibs.capi.lua");
    ASSERT_TRUE(nested.has_value());
    ASSERT_TRUE(nested->namespace_.has_value());
    EXPECT_EQ(*nested->namespace_, "mcpplibs.capi");
    EXPECT_EQ(nested->name, "lua");

    auto nestedCoord = mcpp::pm::normalize_package_selector(*nested);
    EXPECT_EQ(nestedCoord.namespace_, "mcpplibs.capi");
    EXPECT_EQ(nestedCoord.shortName, "lua");
    EXPECT_EQ(mcpp::pm::format_package_selector(nestedCoord),
              "mcpplibs.capi.lua");
}

TEST(DependencySelector, SharedParserRejectsAmbiguousOrUnsafeSegments) {
    const std::vector<std::string> invalid{
        "",
        ".imgui",
        "ocornut..imgui",
        "imgui.",
        "acme/widget",
        "acme\\widget",
        "acme widget",
        "acme[widget",
        "acme=widget",
        "acme\"widget",
        "acme#widget",
        "acme@widget",
        "acme:widget",
        std::string("acme\x1fwidget", 11),
    };
    for (auto const& spelling : invalid) {
        auto parsed = mcpp::pm::parse_package_selector(spelling);
        EXPECT_FALSE(parsed.has_value()) << spelling;
        if (!parsed)
            EXPECT_FALSE(parsed.error().message.empty()) << spelling;
    }
}

TEST(DependencySelector, LegacyDottedPrimaryIsExplicitAndNeverImplicitlyUsed) {
    auto capi = mcpp::pm::parse_package_selector("capi.lua");
    ASSERT_TRUE(capi.has_value());
    auto legacy = mcpp::pm::legacy_prefixed_coordinate(
        mcpp::pm::normalize_package_selector(*capi));
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ(legacy->namespace_, "mcpplibs.capi");
    EXPECT_EQ(legacy->shortName, "lua");

    auto explicitDefault = mcpp::pm::parse_package_selector(
        "mcpplibs.capi.lua");
    ASSERT_TRUE(explicitDefault.has_value());
    EXPECT_FALSE(mcpp::pm::legacy_prefixed_coordinate(
        mcpp::pm::normalize_package_selector(*explicitDefault)).has_value());

    auto bare = mcpp::pm::parse_package_selector("lua");
    ASSERT_TRUE(bare.has_value());
    EXPECT_FALSE(mcpp::pm::legacy_prefixed_coordinate(
        mcpp::pm::normalize_package_selector(*bare)).has_value());
}

// ─── descriptor_coordinates (package-template fetch) ────────────────

TEST(PmCompat, DescriptorCoordinatesLegacyEmbeddedNamespace) {
    // pkgs/l/llmapi.lua: namespace = "mcpplibs", name = "mcpplibs.llmapi"
    auto r = mcpp::pm::compat::descriptor_coordinates(
        "llmapi", "mcpplibs", "mcpplibs.llmapi");

    EXPECT_EQ(r.namespace_, "mcpplibs");
    EXPECT_EQ(r.shortName, "llmapi");
    EXPECT_TRUE(r.usedLegacySplit);
}

TEST(PmCompat, DescriptorCoordinatesCanonicalNamespaceField) {
    auto r = mcpp::pm::compat::descriptor_coordinates(
        "llmapi", "mcpplibs", "llmapi");

    EXPECT_EQ(r.namespace_, "mcpplibs");
    EXPECT_EQ(r.shortName, "llmapi");
    EXPECT_FALSE(r.usedLegacySplit);
}

TEST(PmCompat, DescriptorCoordinatesRootPackageStaysInRoot) {
    // pkgs/i/imgui.lua: namespace = "", name = "imgui" — must NOT be
    // promoted to the default namespace (it installs by its bare name).
    auto r = mcpp::pm::compat::descriptor_coordinates("imgui", "", "imgui");

    EXPECT_EQ(r.namespace_, "");
    EXPECT_EQ(r.shortName, "imgui");
    EXPECT_FALSE(r.usedLegacySplit);
}

TEST(PmCompat, DescriptorCoordinatesLegacyDottedNameWithoutNamespace) {
    auto r = mcpp::pm::compat::descriptor_coordinates(
        "tinyhttps", "", "mcpplibs.tinyhttps");

    EXPECT_EQ(r.namespace_, "mcpplibs");
    EXPECT_EQ(r.shortName, "tinyhttps");
    EXPECT_TRUE(r.usedLegacySplit);
}

TEST(PmCompat, DescriptorCoordinatesFallsBackToSpecWhenNameMissing) {
    auto r = mcpp::pm::compat::descriptor_coordinates("imgui", "", "");

    EXPECT_EQ(r.namespace_, "");
    EXPECT_EQ(r.shortName, "imgui");
}

TEST(PmCompat, DescriptorCoordinatesCompatNamespace) {
    auto r = mcpp::pm::compat::descriptor_coordinates(
        "mbedtls", "compat", "compat.mbedtls");

    EXPECT_EQ(r.namespace_, "compat");
    EXPECT_EQ(r.shortName, "mbedtls");
}
