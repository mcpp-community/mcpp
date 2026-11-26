import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.toolchain.platform;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL. A package's features as one catalogue (mcpp#992, SPEC-004 §16): every output
// that states them is written from `feature_catalog`, so its entries are the contract. The two
// sources a manifest is read from -- `mcpp.toml` and a descriptor's inline table -- give the same
// catalogue for the same statement.

namespace mm = mcpp::manifest;

namespace {

const mm::FeatureInfo* find(const std::vector<mm::FeatureInfo>& list, std::string_view name) {
    auto it = std::ranges::find(list, name, &mm::FeatureInfo::name);
    return it == list.end() ? nullptr : &*it;
}

constexpr auto kToml = R"(
[package]
name = "widget"
version = "0.1.0"
[targets.widget]
kind = "lib"
[dependencies]
imgui = "1.0.0"
[features]
default = ["gui"]
gui     = ["render", "imgui/docking"]
render  = []
extra   = { implies = ["render"], description = "Extra helpers" }
)";

}  // namespace

UTest the_catalogue_states_defaults_implies_enables_and_descriptions{"FeatureCatalog/DefaultsImpliesEnablesDescriptions", [] {
    auto m = mm::parse_string(kToml);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    const auto list = mm::feature_catalog(*m);
    std::vector<std::string> names;
    for (auto const& f : list) names.push_back(f.name);
    expect(eq(names, std::vector<std::string>{"extra", "gui", "render"})) << "not sorted by name, or `default` listed";

    auto const* gui = find(list, "gui");
    expect(fatal(gui != nullptr));
    expect(gui->isDefault);
    expect(eq(gui->implies, std::vector<std::string>{"render"})) << "a forward is not a feature of the package";
    expect(eq(gui->enables, std::vector<std::string>{"imgui/docking"}));
    expect(gui->description.empty()) << "the array shorthand has no description";

    auto const* render = find(list, "render");
    expect(fatal(render != nullptr));
    expect(render->isDefault) << "the default set is its implies closure";

    auto const* extra = find(list, "extra");
    expect(fatal(extra != nullptr));
    expect(!extra->isDefault);
    expect(eq(extra->description, std::string("Extra helpers")));
    expect(m->schemaWarnings.empty()) << (m->schemaWarnings.empty() ? "" : m->schemaWarnings.front());
}};

UTest a_package_without_features_has_an_empty_catalogue{"FeatureCatalog/NoFeatures", [] {
    auto m = mm::parse_string("[package]\nname = \"p\"\nversion = \"0.1.0\"\n[targets.p]\nkind = \"lib\"\n");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(mm::feature_catalog(*m).empty());
}};

UTest a_description_is_a_string{"FeatureCatalog/DescriptionIsAString", [] {
    auto bad = mm::parse_string("[package]\nname = \"p\"\nversion = \"0.1.0\"\n[targets.p]\nkind = \"lib\"\n"
                                "[features]\nx = { description = 3 }\n");
    expect(fatal(!bad.has_value()));
    expect(bad.error().format().contains("features.x.description must be a string")) << bad.error().format();
}};

UTest a_descriptor_table_states_the_same_catalogue{"FeatureCatalog/DescriptorTableSameCatalogue", [] {
    auto m = mm::synthesize_from_xpkg_lua(R"(
package = {
    spec = "1", name = "widget",
    xpm = { linux = { ["0.1.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/*.cppm" },
        deps = { ["imgui"] = "1.0.0" },
        features = {
            ["default"] = { implies = { "gui" } },
            ["gui"] = { implies = { "render", "imgui/docking" } },
            ["render"] = {},
            ["extra"] = { implies = { "render" }, description = "Extra helpers" },
        },
    },
}
)", "widget", "0.1.0", mcpp::toolchain::platform::TargetPlatform::for_os("linux"));
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto fromToml = mm::parse_string(kToml);
    expect(fatal(fromToml.has_value()));
    const auto a = mm::feature_catalog(*m);
    const auto b = mm::feature_catalog(*fromToml);
    expect(m->xpkgUnknownKeys.empty()) << "a descriptor's description is reported as unknown";
    expect(fatal(eq(a.size(), b.size())));
    for (std::size_t i = 0; i < a.size(); ++i) {
        expect(eq(a[i].name, b[i].name));
        expect(eq(a[i].isDefault, b[i].isDefault)) << a[i].name;
        expect(eq(a[i].implies, b[i].implies)) << a[i].name;
        expect(eq(a[i].enables, b[i].enables)) << a[i].name;
        expect(eq(a[i].description, b[i].description)) << a[i].name;
    }
}};

UTest a_descriptor_description_is_a_string{"FeatureCatalog/DescriptorDescriptionIsAString", [] {
    auto bad = mm::synthesize_from_xpkg_lua(R"(
package = {
    spec = "1", name = "w",
    xpm = { linux = { ["0.1.0"] = { url = "u", sha256 = "h" } } },
    mcpp = { sources = { "*/src/*.cppm" }, features = { ["x"] = { description = 3 } } },
}
)", "w", "0.1.0", mcpp::toolchain::platform::TargetPlatform::for_os("linux"));
    expect(fatal(!bad.has_value()));
    expect(bad.error().format().contains("features.x.description must be a string")) << bad.error().format();
}};

// The third source (mcpp#1018): an installed payload answers from the `mcpp.toml` it carries,
// read from its directory, and states what the manifest and the descriptor state.
UTest an_installed_manifest_states_the_same_catalogue{"FeatureCatalog/InstalledManifestSameCatalogue", [] {
    const auto dir = std::filesystem::temp_directory_path()
        / std::format("mcpp-feature-catalog-{}", std::random_device{}());
    std::filesystem::create_directories(dir / "src");
    { std::ofstream(dir / "mcpp.toml") << kToml; }
    { std::ofstream(dir / "src" / "widget.cppm") << "export module widget;\n"; }
    auto installed = mm::load(dir / "mcpp.toml");
    std::filesystem::remove_all(dir);
    if (auto&& failed = expect(fatal(installed.has_value())); !failed) failed << installed.error().format();
    auto fromToml = mm::parse_string(kToml);
    expect(fatal(fromToml.has_value()));
    const auto a = mm::feature_catalog(*installed);
    const auto b = mm::feature_catalog(*fromToml);
    expect(fatal(eq(a.size(), b.size())));
    for (std::size_t i = 0; i < a.size(); ++i) expect(eq(a[i].description, b[i].description)) << a[i].name;
}};
