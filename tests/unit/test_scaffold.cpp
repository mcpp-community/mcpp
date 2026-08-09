#include <gtest/gtest.h>

import std;
import mcpp.scaffold;
import mcpp.pm.dependency_selector;

namespace {

TEST(TemplateSpec, ParsesSharedExactPackageGrammar) {
    struct Row {
        std::string input;
        std::optional<std::string> ns;
        std::string name;
        std::optional<std::string> version;
        std::optional<std::string> tmpl;
    };
    const std::vector<Row> rows{
        {"imgui", std::nullopt, "imgui", std::nullopt, std::nullopt},
        {"ocornut.imgui", "ocornut", "imgui", std::nullopt, std::nullopt},
        {"ocornut.imgui@1.92.8", "ocornut", "imgui", "1.92.8", std::nullopt},
        {"ocornut.imgui:glfw-opengl3", "ocornut", "imgui", std::nullopt,
         "glfw-opengl3"},
        {"ocornut.imgui@1.92.8:vulkan", "ocornut", "imgui", "1.92.8",
         "vulkan"},
        {"mcpplibs.gui.templates@2.0.0:window", "mcpplibs.gui", "templates",
         "2.0.0", "window"},
        {"pkg@1.0.0-rc.1+build.7:t_1", std::nullopt, "pkg",
         "1.0.0-rc.1+build.7", "t_1"},
    };

    for (auto const& row : rows) {
        auto parsed = mcpp::scaffold::parse_template_spec(row.input);
        ASSERT_TRUE(parsed.has_value()) << row.input;
        EXPECT_EQ(parsed->package.namespace_, row.ns) << row.input;
        EXPECT_EQ(parsed->package.name, row.name) << row.input;
        EXPECT_EQ(parsed->version, row.version) << row.input;
        EXPECT_EQ(parsed->templateName, row.tmpl) << row.input;
        EXPECT_FALSE(parsed->legacyList) << row.input;
    }
}

TEST(TemplateSpec, BarePackageNormalizesToMcpplibs) {
    auto parsed = mcpp::scaffold::parse_template_spec("imgui");
    ASSERT_TRUE(parsed.has_value());
    auto coord = mcpp::pm::normalize_package_selector(parsed->package);
    EXPECT_EQ(coord.namespace_, "mcpplibs");
    EXPECT_EQ(coord.shortName, "imgui");
}

TEST(TemplateSpec, RecognizesTrailingColonOnlyAsLegacyListAlias) {
    auto parsed = mcpp::scaffold::parse_template_spec("ocornut.imgui@1.92.8:");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->legacyList);
    EXPECT_EQ(parsed->version, "1.92.8");
    EXPECT_FALSE(parsed->templateName.has_value());
}

TEST(TemplateSpec, RejectsAmbiguousEmptyOrUnsafeComponents) {
    const std::vector<std::string> invalid{
        "", "@1.0.0", ":starter", ".imgui@1.0", "ocornut..imgui",
        "ocornut.imgui@", "ocornut.imgui:bad:extra",
        "ocornut.imgui@1.0@2.0", "ocornut.imgui@^1.0",
        "ocornut.imgui@*", "ocornut.imgui:bad.name",
        "ocornut.imgui:bad/name", "ocornut.imgui@1.0 bad",
        std::string("ocornut.imgui:\x1f", 16),
    };
    for (auto const& input : invalid) {
        auto parsed = mcpp::scaffold::parse_template_spec(input);
        EXPECT_FALSE(parsed.has_value()) << input;
        if (!parsed) EXPECT_FALSE(parsed.error().message.empty()) << input;
    }
}

TEST(TemplateSelection, SoleTemplateWithoutExplicitDefaultAutoWins) {
    std::vector<mcpp::scaffold::TemplateEntry> entries{
        {"solo", {.description = "only choice", .isDefault = false}},
    };
    auto chosen = mcpp::scaffold::select_template(entries, std::nullopt);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ((*chosen)->name, "solo");
}

TEST(TemplateSelection, OneExplicitDefaultWinsAmongMany) {
    std::vector<mcpp::scaffold::TemplateEntry> entries{
        {"a", {.isDefault = false}},
        {"b", {.isDefault = true}},
    };
    auto chosen = mcpp::scaffold::select_template(entries, std::nullopt);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ((*chosen)->name, "b");
}

TEST(TemplateSelection, MultipleWithoutDefaultAreAmbiguous) {
    std::vector<mcpp::scaffold::TemplateEntry> entries{
        {"a", {.isDefault = false}},
        {"b", {.isDefault = false}},
    };
    auto chosen = mcpp::scaffold::select_template(entries, std::nullopt);
    ASSERT_FALSE(chosen.has_value());
    EXPECT_NE(chosen.error().find("a"), std::string::npos);
    EXPECT_NE(chosen.error().find("b"), std::string::npos);
}

TEST(TemplateSelection, ExplicitNameWinsAndUnknownListsChoices) {
    std::vector<mcpp::scaffold::TemplateEntry> entries{
        {"a", {.isDefault = false}},
        {"b", {.isDefault = false}},
    };
    auto chosen = mcpp::scaffold::select_template(entries, std::string_view{"b"});
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ((*chosen)->name, "b");

    auto missing = mcpp::scaffold::select_template(
        entries, std::string_view{"missing"});
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().find("a"), std::string::npos);
    EXPECT_NE(missing.error().find("b"), std::string::npos);
}

TEST(TemplateSelection, EmptyOrMultipleDefaultsAreProviderErrors) {
    std::vector<mcpp::scaffold::TemplateEntry> empty;
    auto noTemplates = mcpp::scaffold::select_template(empty, std::nullopt);
    ASSERT_FALSE(noTemplates.has_value());
    EXPECT_NE(noTemplates.error().find("no templates"), std::string::npos);

    std::vector<mcpp::scaffold::TemplateEntry> entries{
        {"a", {.isDefault = true}},
        {"b", {.isDefault = true}},
    };
    auto duplicate = mcpp::scaffold::select_template(entries, std::nullopt);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_NE(duplicate.error().find("more than one default"),
              std::string::npos);
}

} // namespace
