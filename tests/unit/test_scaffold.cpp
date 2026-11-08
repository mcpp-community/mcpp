
import std;
import mcpplibs.mtest;
import mcpp.scaffold;
import mcpp.scaffold.project_name;
import mcpp.pm.dependency_selector;
import mcpplibs.os.fs;

using namespace mcpplibs::mtest;

namespace {

UTest parses_shared_exact_package_grammar{"TemplateSpec/ParsesSharedExactPackageGrammar", [] {
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
        expect(fatal(parsed.has_value())) << row.input;
        expect(eq(parsed->package.namespace_, row.ns)) << row.input;
        expect(eq(parsed->package.name, row.name)) << row.input;
        expect(eq(parsed->version, row.version)) << row.input;
        expect(eq(parsed->templateName, row.tmpl)) << row.input;
        expect(!parsed->legacyList) << row.input;
    }
}};

UTest bare_package_normalizes_to_mcpplibs{"TemplateSpec/BarePackageNormalizesToMcpplibs", [] {
    auto parsed = mcpp::scaffold::parse_template_spec("imgui");
    expect(fatal(parsed.has_value()));
    auto coord = mcpp::pm::normalize_package_selector(parsed->package);
    expect(eq(coord.namespace_, "mcpplibs"));
    expect(eq(coord.shortName, "imgui"));
}};

UTest recognizes_trailing_colon_only_as_legacy_list_alias{"TemplateSpec/RecognizesTrailingColonOnlyAsLegacyListAlias", [] {
    auto parsed = mcpp::scaffold::parse_template_spec("ocornut.imgui@1.92.8:");
    expect(fatal(parsed.has_value()));
    expect(bool(parsed->legacyList));
    expect(eq(parsed->version, "1.92.8"));
    expect(!(parsed->templateName.has_value()));
}};

UTest rejects_ambiguous_empty_or_unsafe_components{"TemplateSpec/RejectsAmbiguousEmptyOrUnsafeComponents", [] {
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
        expect(!parsed.has_value()) << input;
        if (!parsed) expect(!(parsed.error().message.empty())) << input;
    }
}};

UTest sole_template_without_explicit_default_auto_wins{"TemplateSelection/SoleTemplateWithoutExplicitDefaultAutoWins", [] {
    std::vector<mcpp::scaffold::TemplateEntry> entries{
        {"solo", {.description = "only choice", .isDefault = false}},
    };
    auto chosen = mcpp::scaffold::select_template(entries, std::nullopt);
    expect(fatal(chosen.has_value()));
    expect(eq((*chosen)->name, "solo"));
}};

UTest one_explicit_default_wins_among_many{"TemplateSelection/OneExplicitDefaultWinsAmongMany", [] {
    std::vector<mcpp::scaffold::TemplateEntry> entries{
        {"a", {.isDefault = false}},
        {"b", {.isDefault = true}},
    };
    auto chosen = mcpp::scaffold::select_template(entries, std::nullopt);
    expect(fatal(chosen.has_value()));
    expect(eq((*chosen)->name, "b"));
}};

UTest multiple_without_default_are_ambiguous{"TemplateSelection/MultipleWithoutDefaultAreAmbiguous", [] {
    std::vector<mcpp::scaffold::TemplateEntry> entries{
        {"a", {.isDefault = false}},
        {"b", {.isDefault = false}},
    };
    auto chosen = mcpp::scaffold::select_template(entries, std::nullopt);
    expect(fatal(!chosen.has_value()));
    expect(ne(chosen.error().find("a"), std::string::npos));
    expect(ne(chosen.error().find("b"), std::string::npos));
}};

UTest explicit_name_wins_and_unknown_lists_choices{"TemplateSelection/ExplicitNameWinsAndUnknownListsChoices", [] {
    std::vector<mcpp::scaffold::TemplateEntry> entries{
        {"a", {.isDefault = false}},
        {"b", {.isDefault = false}},
    };
    auto chosen = mcpp::scaffold::select_template(entries, std::string_view{"b"});
    expect(fatal(chosen.has_value()));
    expect(eq((*chosen)->name, "b"));

    auto missing = mcpp::scaffold::select_template(
        entries, std::string_view{"missing"});
    expect(fatal(!missing.has_value()));
    expect(ne(missing.error().find("a"), std::string::npos));
    expect(ne(missing.error().find("b"), std::string::npos));
}};

UTest empty_or_multiple_defaults_are_provider_errors{"TemplateSelection/EmptyOrMultipleDefaultsAreProviderErrors", [] {
    std::vector<mcpp::scaffold::TemplateEntry> empty;
    auto noTemplates = mcpp::scaffold::select_template(empty, std::nullopt);
    expect(fatal(!noTemplates.has_value()));
    expect(ne(noTemplates.error().find("no templates"), std::string::npos));

    std::vector<mcpp::scaffold::TemplateEntry> entries{
        {"a", {.isDefault = true}},
        {"b", {.isDefault = true}},
    };
    auto duplicate = mcpp::scaffold::select_template(entries, std::nullopt);
    expect(fatal(!duplicate.has_value()));
    expect(ne(duplicate.error().find("more than one default"), std::string::npos));
}};

UTest accepts_portable_bare_and_qualified_names{"ProjectName/AcceptsPortableBareAndQualifiedNames", [] {
    auto bare = mcpp::scaffold::validate_project_name("hello-world");
    expect(fatal(bare.has_value()));
    expect(eq(bare->directoryName, "hello-world"));
    expect(eq(bare->name, "hello-world"));
    expect(bare->namespace_.empty());
    expect(eq(bare->qualifiedName, "hello-world"));

    auto qualified = mcpp::scaffold::validate_project_name("acme.tools.app");
    expect(fatal(qualified.has_value()));
    expect(eq(qualified->directoryName, "acme.tools.app"));
    expect(eq(qualified->namespace_, "acme.tools"));
    expect(eq(qualified->name, "app"));
    expect(eq(qualified->qualifiedName, "acme.tools.app"));
}};

UTest rejects_path_escape_controls_reserved_names_and_legacy_marker{"ProjectName/RejectsPathEscapeControlsReservedNamesAndLegacyMarker", [] {
    const std::vector<std::string> invalid{
        "", ".", "..", "/absolute", "a/b", "a\\b", "a:b", "a\"b",
        "a?b", "a*b", "a|b", "a<b", "a>b", "trail.", "trail ",
        "CON", "con.txt", "aux", "NUL.log", "COM1", "com9.ext", "LPT1",
        "lpt9.txt", "PROJECT", "myPROJECTname", "bad\tname",
        std::string("bad\x1f", 4), std::string("bad\x7f", 4),
    };
    for (auto const& name : invalid) {
        auto result = mcpp::scaffold::validate_project_name(name);
        expect(!result.has_value()) << name;
        if (!result) expect(!(result.error().message.empty())) << name;
    }
}};

UTest expands_complete_vocabulary_without_rescanning_values{"RenderTokens/ExpandsCompleteVocabularyWithoutRescanningValues", [] {
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
    if (auto&& failed = expect(fatal(rendered.has_value())); !failed) failed << rendered.error().message;
    expect(eq(*rendered, "app-{{template.name}}|acme|acme.app|widgets.ui|starter|"
        "widgets.ui.starter|2.0.0|window|widgets.ui.starter|2.0.0"));
}};

UTest rejects_unknown_or_unterminated_tokens{"RenderTokens/RejectsUnknownOrUnterminatedTokens", [] {
    mcpp::scaffold::RenderVars vars;
    auto unknown = mcpp::scaffold::render_tokens("{{project.typo}}", vars);
    expect(fatal(!unknown.has_value()));
    expect(ne(unknown.error().message.find("project.typo"), std::string::npos));

    auto unterminated = mcpp::scaffold::render_tokens("{{project.name", vars);
    expect(fatal(!unterminated.has_value()));
    expect(ne(unterminated.error().message.find("unterminated"), std::string::npos));
}};

UTest rollback_removes_sibling_staging_tree{"ScaffoldTransaction/RollbackRemovesSiblingStagingTree", [] {
    auto parent = std::filesystem::temp_directory_path()
        / std::format("mcpp-scaffold-rollback-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(parent);
    std::filesystem::path stage;
    {
        auto tx = mcpp::scaffold::ScaffoldTransaction::begin(parent, "app");
        if (auto&& failed = expect(fatal(tx.has_value())); !failed) failed << tx.error();
        stage = tx->staging_path();
        std::ofstream(stage / "partial.txt") << "partial";
        expect(std::filesystem::exists(stage));
    }
    expect(!std::filesystem::exists(stage));
    expect(!std::filesystem::exists(parent / "app"));
    std::filesystem::remove_all(parent);
}};

UTest commit_is_atomic_and_rename_failure_rolls_back{"ScaffoldTransaction/CommitIsAtomicAndRenameFailureRollsBack", [] {
    auto parent = std::filesystem::temp_directory_path()
        / std::format("mcpp-scaffold-commit-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(parent);
    {
        auto tx = mcpp::scaffold::ScaffoldTransaction::begin(parent, "ok");
        if (auto&& failed = expect(fatal(tx.has_value())); !failed) failed << tx.error();
        std::ofstream(tx->staging_path() / "done.txt") << "done";
        auto committed = tx->commit();
        if (auto&& failed = expect(fatal(committed.has_value())); !failed) failed << committed.error();
        expect(std::filesystem::exists(parent / "ok" / "done.txt"));
        expect(!(std::filesystem::exists(tx->staging_path())));
    }
    {
        auto tx = mcpp::scaffold::ScaffoldTransaction::begin(parent, "collision");
        if (auto&& failed = expect(fatal(tx.has_value())); !failed) failed << tx.error();
        auto stage = tx->staging_path();
        std::ofstream(stage / "partial.txt") << "partial";
        std::filesystem::create_directories(parent / "collision");
        auto committed = tx->commit();
        expect(fatal(!committed.has_value()));
        expect(std::filesystem::exists(stage));
    }
    for (auto const& entry : std::filesystem::directory_iterator(parent)) {
        expect(!(entry.path().filename().string().starts_with(".mcpp-new-")));
    }
    std::filesystem::remove_all(parent);
}};

UTest atomic_rename_never_replaces_existing_target{"ScaffoldFilesystem/AtomicRenameNeverReplacesExistingTarget", [] {
    auto parent = std::filesystem::temp_directory_path()
        / std::format("mcpp-scaffold-rename-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    auto source = parent / "source";
    auto target = parent / "target";
    std::filesystem::create_directories(source);
    std::filesystem::create_directories(target);
    std::ofstream(source / "payload.txt") << "payload";
    std::ofstream(target / "owner.txt") << "owner";

    auto collision = mcpplibs::os::fs::place(
        source, target);
    expect(fatal(!collision.has_value()));
    expect(std::filesystem::exists(source / "payload.txt"));
    expect(std::filesystem::exists(target / "owner.txt"));

    std::filesystem::remove_all(target);
    auto committed = mcpplibs::os::fs::place(
        source, target);
    if (auto&& failed = expect(fatal(committed.has_value())); !failed) failed << committed.error().message();
    expect(!std::filesystem::exists(source));
    expect(std::filesystem::exists(target / "payload.txt"));
    std::filesystem::remove_all(parent);
}};

#if !defined(_WIN32)
UTest read_write_and_copy_failures_rollback_completely{"ScaffoldTransaction/ReadWriteAndCopyFailuresRollbackCompletely", [] {
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
        if (auto&& failed = expect(fatal(tx.has_value())); !failed) failed << tx.error();
        readStage = tx->staging_path();
        auto result = mcpp::scaffold::instantiate(
            readProvider, readStage, vars);
        expect(!result.has_value());
        fs::permissions(unreadable, fs::perms::owner_read);
    }
    expect(!fs::exists(readStage));
    expect(!fs::exists(parent / "read-app"));

    auto exercise_locked_destination = [&](std::string_view caseName,
                                            std::string_view sourceName) {
        auto provider = providers / std::string(caseName);
        fs::create_directories(provider / "locked");
        std::ofstream(provider / "locked" / std::string(sourceName)) << "data";
        fs::path stage;
        {
            auto tx = mcpp::scaffold::ScaffoldTransaction::begin(
                parent, std::format("{}-app", caseName));
            expect(tx.has_value());
            if (!tx) return;
            stage = tx->staging_path();
            fs::create_directories(stage / "locked");
            fs::permissions(stage / "locked",
                            fs::perms::owner_read | fs::perms::owner_exec);
            auto result = mcpp::scaffold::instantiate(provider, stage, vars);
            expect(!result.has_value()) << caseName;
            fs::permissions(stage / "locked", fs::perms::owner_all);
        }
        expect(!fs::exists(stage)) << caseName;
        expect(!(fs::exists(parent / std::format("{}-app", caseName))))
            << caseName;
    };
    exercise_locked_destination("write", "mcpp.toml.in");
    exercise_locked_destination("copy", "data.bin");

    for (auto const& entry : fs::directory_iterator(parent)) {
        expect(!(entry.path().filename().string().starts_with(".mcpp-new-")));
    }
    fs::remove_all(parent);
}};
#endif

UTest compares_canonical_package_identity{"SelfDependencyInjection/ComparesCanonicalPackageIdentity", [] {
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
    if (auto&& failed = expect(fatal(injected.has_value())); !failed) failed << injected.error();
    std::string text;
    {
        std::ifstream is(manifest);
        std::stringstream ss;
        ss << is.rdbuf();
        text = ss.str();
    }
    expect(ne(text.find("[dependencies.acme]\nwidget = { version = \"2.0.0\", features = [\"gui\"] }"), std::string::npos));
    expect(ne(text.find("[dependencies.compat]\nwidget = \"1.0.0\""), std::string::npos));

    auto again = mcpp::scaffold::inject_self_dependency(
        manifest, vars, {"gui"});
    if (auto&& failed = expect(fatal(again.has_value())); !failed) failed << again.error();
    {
        std::ifstream is(manifest);
        std::stringstream ss;
        ss << is.rdbuf();
        expect(eq(ss.str(), text));
    }
    std::filesystem::remove_all(root);
}};

} // namespace

// ── [template.inject] self = false ───────────────────────────────────────────
//
// A TEMPLATE THAT MUST NOT DEPEND ON ITS OWN PACKAGE IS NOT HYPOTHETICAL.
//
// The self-dependency keeps a template from drifting from the library that
// ships it, and for almost every template that is right. It is wrong for one
// whose subject is depending on NOTHING: a bare-metal package may ship a kernel
// starting point for a target with no C library, while the package's own module
// includes <stdio.h>. Measured: the injected dependency failed to compile
// before the generated project was reached, in a project whose manifest
// declares no dependencies at all.
UTest inject_self_can_be_declined{"ScaffoldTemplate/InjectSelfCanBeDeclined", [] {
    auto dir = std::filesystem::temp_directory_path() / "mcpp_tmpl_noself";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "template.toml");
        f << "[template]\ndescription = \"d\"\n[template.inject]\nself = false\n";
    }
    auto meta = mcpp::scaffold::load_meta(dir);
    if (auto&& failed = expect(fatal(meta.has_value())); !failed) failed << (meta ? "" : meta.error());
    expect(!meta->injectSelf);
    std::filesystem::remove_all(dir);
}};

// The default must stay ON: every template that does not say otherwise relies
// on the injection, and a silent flip would leave them all without their own
// package.
UTest inject_self_defaults_to_on{"ScaffoldTemplate/InjectSelfDefaultsToOn", [] {
    auto dir = std::filesystem::temp_directory_path() / "mcpp_tmpl_self_default";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "template.toml");
        f << "[template]\ndescription = \"d\"\n";
    }
    auto meta = mcpp::scaffold::load_meta(dir);
    if (auto&& failed = expect(fatal(meta.has_value())); !failed) failed << (meta ? "" : meta.error());
    expect(bool(meta->injectSelf));
    std::filesystem::remove_all(dir);
}};

// The table form still carries features, and must not be read as "declined".
UTest inject_self_table_form_still_injects{"ScaffoldTemplate/InjectSelfTableFormStillInjects", [] {
    auto dir = std::filesystem::temp_directory_path() / "mcpp_tmpl_self_table";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "template.toml");
        f << "[template]\ndescription = \"d\"\n"
             "[template.inject]\nself = { features = [\"x\"] }\n";
    }
    auto meta = mcpp::scaffold::load_meta(dir);
    if (auto&& failed = expect(fatal(meta.has_value())); !failed) failed << (meta ? "" : meta.error());
    expect(bool(meta->injectSelf));
    expect(fatal(eq(meta->injectSelfFeatures.size(), 1u)));
    expect(eq(meta->injectSelfFeatures[0], "x"));
    std::filesystem::remove_all(dir);
}};
