#include <gtest/gtest.h>

import std;
import mcpp.platform.scaffold_fs;
import mcpp.scaffold;
import mcpp.scaffold.project_name;
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

TEST(ProjectName, AcceptsPortableBareAndQualifiedNames) {
    auto bare = mcpp::scaffold::validate_project_name("hello-world");
    ASSERT_TRUE(bare.has_value());
    EXPECT_EQ(bare->directoryName, "hello-world");
    EXPECT_EQ(bare->name, "hello-world");
    EXPECT_TRUE(bare->namespace_.empty());
    EXPECT_EQ(bare->qualifiedName, "hello-world");

    auto qualified = mcpp::scaffold::validate_project_name("acme.tools.app");
    ASSERT_TRUE(qualified.has_value());
    EXPECT_EQ(qualified->directoryName, "acme.tools.app");
    EXPECT_EQ(qualified->namespace_, "acme.tools");
    EXPECT_EQ(qualified->name, "app");
    EXPECT_EQ(qualified->qualifiedName, "acme.tools.app");
}

TEST(ProjectName, RejectsPathEscapeControlsReservedNamesAndLegacyMarker) {
    const std::vector<std::string> invalid{
        "", ".", "..", "/absolute", "a/b", "a\\b", "a:b", "a\"b",
        "a?b", "a*b", "a|b", "a<b", "a>b", "trail.", "trail ",
        "CON", "con.txt", "aux", "NUL.log", "COM1", "com9.ext", "LPT1",
        "lpt9.txt", "PROJECT", "myPROJECTname", "bad\tname",
        std::string("bad\x1f", 4), std::string("bad\x7f", 4),
    };
    for (auto const& name : invalid) {
        auto result = mcpp::scaffold::validate_project_name(name);
        EXPECT_FALSE(result.has_value()) << name;
        if (!result) EXPECT_FALSE(result.error().message.empty()) << name;
    }
}

TEST(RenderTokens, ExpandsCompleteVocabularyWithoutRescanningValues) {
    mcpp::scaffold::RenderVars vars{
        .projectName = "app-{{template.name}}",
        .projectNamespace = "acme",
        .projectQualifiedName = "acme.app",
        .templatePackageNamespace = "widgets.ui",
        .templatePackageName = "starter",
        .templatePackageSelector = "widgets.ui.starter",
        .templatePackageVersion = "2.0.0",
        .templateName = "window",
    };
    auto rendered = mcpp::scaffold::render_tokens(
        "{{project.name}}|{{project.namespace}}|{{project.qualifiedName}}|"
        "{{template.package.namespace}}|{{template.package.name}}|"
        "{{template.package.selector}}|{{template.package.version}}|"
        "{{template.name}}|{{self.name}}|{{self.version}}",
        vars);
    ASSERT_TRUE(rendered.has_value()) << rendered.error().message;
    EXPECT_EQ(*rendered,
        "app-{{template.name}}|acme|acme.app|widgets.ui|starter|"
        "widgets.ui.starter|2.0.0|window|widgets.ui.starter|2.0.0");
}

TEST(RenderTokens, RejectsUnknownOrUnterminatedTokens) {
    mcpp::scaffold::RenderVars vars;
    auto unknown = mcpp::scaffold::render_tokens("{{project.typo}}", vars);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_NE(unknown.error().message.find("project.typo"), std::string::npos);

    auto unterminated = mcpp::scaffold::render_tokens("{{project.name", vars);
    ASSERT_FALSE(unterminated.has_value());
    EXPECT_NE(unterminated.error().message.find("unterminated"),
              std::string::npos);
}

TEST(ScaffoldTransaction, RollbackRemovesSiblingStagingTree) {
    auto parent = std::filesystem::temp_directory_path()
        / std::format("mcpp-scaffold-rollback-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(parent);
    std::filesystem::path stage;
    {
        auto tx = mcpp::scaffold::ScaffoldTransaction::begin(parent, "app");
        ASSERT_TRUE(tx.has_value()) << tx.error();
        stage = tx->staging_path();
        std::ofstream(stage / "partial.txt") << "partial";
        EXPECT_TRUE(std::filesystem::exists(stage));
    }
    EXPECT_FALSE(std::filesystem::exists(stage));
    EXPECT_FALSE(std::filesystem::exists(parent / "app"));
    std::filesystem::remove_all(parent);
}

TEST(ScaffoldTransaction, CommitIsAtomicAndRenameFailureRollsBack) {
    auto parent = std::filesystem::temp_directory_path()
        / std::format("mcpp-scaffold-commit-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(parent);
    {
        auto tx = mcpp::scaffold::ScaffoldTransaction::begin(parent, "ok");
        ASSERT_TRUE(tx.has_value()) << tx.error();
        std::ofstream(tx->staging_path() / "done.txt") << "done";
        auto committed = tx->commit();
        ASSERT_TRUE(committed.has_value()) << committed.error();
        EXPECT_TRUE(std::filesystem::exists(parent / "ok" / "done.txt"));
        EXPECT_FALSE(std::filesystem::exists(tx->staging_path()));
    }
    {
        auto tx = mcpp::scaffold::ScaffoldTransaction::begin(parent, "collision");
        ASSERT_TRUE(tx.has_value()) << tx.error();
        auto stage = tx->staging_path();
        std::ofstream(stage / "partial.txt") << "partial";
        std::filesystem::create_directories(parent / "collision");
        auto committed = tx->commit();
        ASSERT_FALSE(committed.has_value());
        EXPECT_TRUE(std::filesystem::exists(stage));
    }
    for (auto const& entry : std::filesystem::directory_iterator(parent)) {
        EXPECT_FALSE(entry.path().filename().string().starts_with(".mcpp-new-"));
    }
    std::filesystem::remove_all(parent);
}

TEST(ScaffoldFilesystem, AtomicRenameNeverReplacesExistingTarget) {
    auto parent = std::filesystem::temp_directory_path()
        / std::format("mcpp-scaffold-rename-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    auto source = parent / "source";
    auto target = parent / "target";
    std::filesystem::create_directories(source);
    std::filesystem::create_directories(target);
    std::ofstream(source / "payload.txt") << "payload";
    std::ofstream(target / "owner.txt") << "owner";

    auto collision = mcpp::platform::atomic_rename_directory_no_replace(
        source, target);
    ASSERT_FALSE(collision.has_value());
    EXPECT_TRUE(std::filesystem::exists(source / "payload.txt"));
    EXPECT_TRUE(std::filesystem::exists(target / "owner.txt"));

    std::filesystem::remove_all(target);
    auto committed = mcpp::platform::atomic_rename_directory_no_replace(
        source, target);
    ASSERT_TRUE(committed.has_value()) << committed.error();
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_TRUE(std::filesystem::exists(target / "payload.txt"));
    std::filesystem::remove_all(parent);
}

#if !defined(_WIN32)
TEST(ScaffoldTransaction, ReadWriteAndCopyFailuresRollbackCompletely) {
    namespace fs = std::filesystem;
    auto parent = fs::temp_directory_path()
        / std::format("mcpp-scaffold-iofail-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    auto providers = parent / "providers";
    fs::create_directories(providers);
    mcpp::scaffold::RenderVars vars{.projectName = "app"};

    // Deterministic read failure: the test process cannot open the rendered
    // source. Restore permissions before cleanup.
    auto readProvider = providers / "read";
    fs::create_directories(readProvider);
    auto unreadable = readProvider / "mcpp.toml.in";
    std::ofstream(unreadable) << "[package]\nname=\"app\"\nversion=\"0.1.0\"\n";
    fs::permissions(unreadable, fs::perms::none);
    fs::path readStage;
    {
        auto tx = mcpp::scaffold::ScaffoldTransaction::begin(parent, "read-app");
        ASSERT_TRUE(tx.has_value()) << tx.error();
        readStage = tx->staging_path();
        auto result = mcpp::scaffold::instantiate(
            readProvider, readStage, vars);
        EXPECT_FALSE(result.has_value());
        fs::permissions(unreadable, fs::perms::owner_read);
    }
    EXPECT_FALSE(fs::exists(readStage));
    EXPECT_FALSE(fs::exists(parent / "read-app"));

    auto exercise_locked_destination = [&](std::string_view caseName,
                                            std::string_view sourceName) {
        auto provider = providers / std::string(caseName);
        fs::create_directories(provider / "locked");
        std::ofstream(provider / "locked" / std::string(sourceName)) << "data";
        fs::path stage;
        {
            auto tx = mcpp::scaffold::ScaffoldTransaction::begin(
                parent, std::format("{}-app", caseName));
            EXPECT_TRUE(tx.has_value());
            if (!tx) return;
            stage = tx->staging_path();
            fs::create_directories(stage / "locked");
            fs::permissions(stage / "locked",
                            fs::perms::owner_read | fs::perms::owner_exec);
            auto result = mcpp::scaffold::instantiate(provider, stage, vars);
            EXPECT_FALSE(result.has_value()) << caseName;
            fs::permissions(stage / "locked", fs::perms::owner_all);
        }
        EXPECT_FALSE(fs::exists(stage)) << caseName;
        EXPECT_FALSE(fs::exists(parent / std::format("{}-app", caseName)))
            << caseName;
    };
    exercise_locked_destination("write", "mcpp.toml.in");
    exercise_locked_destination("copy", "data.bin");

    for (auto const& entry : fs::directory_iterator(parent)) {
        EXPECT_FALSE(entry.path().filename().string().starts_with(".mcpp-new-"));
    }
    fs::remove_all(parent);
}
#endif

TEST(SelfDependencyInjection, ComparesCanonicalPackageIdentity) {
    auto root = std::filesystem::temp_directory_path()
        / std::format("mcpp-inject-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    auto manifest = root / "mcpp.toml";
    {
        std::ofstream os(manifest);
        os << R"([package]
name = "app"
version = "0.1.0"

[dependencies.compat]
widget = "1.0.0"
)";
    }
    mcpp::scaffold::RenderVars vars{
        .templatePackageNamespace = "acme",
        .templatePackageName = "widget",
        .templatePackageSelector = "acme.widget",
        .templatePackageVersion = "2.0.0",
    };
    auto injected = mcpp::scaffold::inject_self_dependency(
        manifest, vars, {"gui"});
    ASSERT_TRUE(injected.has_value()) << injected.error();
    std::string text;
    {
        std::ifstream is(manifest);
        std::stringstream ss;
        ss << is.rdbuf();
        text = ss.str();
    }
    EXPECT_NE(text.find("[dependencies.acme]\nwidget = { version = \"2.0.0\", features = [\"gui\"] }"),
              std::string::npos);
    EXPECT_NE(text.find("[dependencies.compat]\nwidget = \"1.0.0\""),
              std::string::npos);

    auto again = mcpp::scaffold::inject_self_dependency(
        manifest, vars, {"gui"});
    ASSERT_TRUE(again.has_value()) << again.error();
    {
        std::ifstream is(manifest);
        std::stringstream ss;
        ss << is.rdbuf();
        EXPECT_EQ(ss.str(), text);
    }
    std::filesystem::remove_all(root);
}

} // namespace
