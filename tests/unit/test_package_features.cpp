import std;
import mcpplibs.mtest;
import mcpp.pm.package_manifest;
import mcpp.toolchain.platform;

using namespace mcpplibs::mtest;

// CROSS-LAYER: the answer for one version of an index package (mcpp#992, SPEC-004 §16): the
// descriptor's two forms (SPEC-001 §3.5), the store, and the manifest the build reads, met in
// `package_features`. The e2e suite states the same against an index (tests/e2e/index/features.cpp).

namespace fs = std::filesystem;
namespace pm = mcpp::pm;

namespace {

const auto kLinux = mcpp::toolchain::platform::TargetPlatform::for_os("linux");

constexpr auto kPointer = R"(package = {
    spec = "1", namespace = "acme", name = "widget",
    xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = "*/mcpp.toml",
})";

constexpr auto kInline = R"(package = {
    spec = "1", namespace = "acme", name = "widget",
    xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = { sources = { "*/src/*.cppm" }, features = { ["gui"] = { description = "A window" } } },
})";

struct Store {
    fs::path dir = fs::temp_directory_path() / std::format("mcpp_package_features_{}", std::random_device{}());
    Store() { fs::create_directories(dir); }
    ~Store() { std::error_code ec; fs::remove_all(dir, ec); }
    void write(const fs::path& rel, std::string_view text) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel) << text;
    }
};

}  // namespace

UTest an_inline_descriptor_answers_from_the_index{"PackageFeatures/InlineAnswersFromTheIndex", [] {
    // No version directory: the table is the manifest, and nothing need be installed.
    const auto a = pm::package_features(kInline, {}, "widget", "1.0.0", kLinux);
    expect(eq(a.state, std::string("declared")));
    expect(eq(a.from, std::string("index")));
    expect(fatal(eq(a.list.size(), 1u)));
    expect(eq(a.list[0].description, std::string("A window")));
}};

UTest a_payload_the_store_lacks_is_not_installed{"PackageFeatures/AbsentPayloadIsNotInstalled", [] {
    Store store;
    for (auto const& dir : {fs::path{}, store.dir / "missing", store.dir}) {
        const auto a = pm::package_features(kPointer, dir, "widget", "1.0.0", kLinux);
        expect(eq(a.state, std::string("not-installed"))) << dir.string();
        expect(eq(a.from, std::string("payload")));
        expect(a.list.empty());
        const auto j = pm::feature_answer_json(a);
        expect(j["list"].is_null()) << "an unknown answer read as no features: " << j.dump();
    }
}};

UTest an_installed_payload_answers_as_the_build_reads_it{"PackageFeatures/InstalledPayloadAnswers", [] {
    // The manifest is a member of a workspace inside the archive, and receives its inheritance as
    // the build's graph does (#690).
    Store store;
    store.write("widget-1.0.0/mcpp.toml", "[workspace]\nmembers = [\"core\"]\n\n[workspace.package]\nlicense = \"MIT\"\n");
    store.write("widget-1.0.0/core/mcpp.toml",
                "[package]\nname = \"widget\"\nversion = \"1.0.0\"\n\n[targets.widget]\nkind = \"lib\"\n\n"
                "[features]\ndefault = [\"gui\"]\ngui = { description = \"A window\" }\n");
    constexpr auto lua = R"(package = {
    spec = "1", namespace = "acme", name = "widget",
    xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = "*/core/mcpp.toml",
})";
    const auto m = pm::installed_manifest(lua, store.dir, "widget", "1.0.0", kLinux);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format("widget");
    expect(eq(m->manifest.package.license, std::string("MIT"))) << "the workspace's inheritance was not applied";
    expect(eq(m->manifest.package.namespace_, std::string("acme"))) << "the descriptor's namespace was not applied";

    const auto a = pm::package_features(lua, store.dir, "widget", "1.0.0", kLinux);
    expect(eq(a.state, std::string("declared")));
    expect(eq(a.from, std::string("payload")));
    expect(fatal(eq(a.list.size(), 1u)));
    expect(a.list[0].isDefault);
    const auto j = pm::feature_answer_json(a);
    expect(eq(j["list"][0]["description"].get<std::string>(), std::string("A window")));
}};

UTest a_manifest_that_cannot_be_read_is_unreadable{"PackageFeatures/BrokenManifestIsUnreadable", [] {
    Store store;
    store.write("widget-1.0.0/mcpp.toml", "[package\nname = ");
    const auto a = pm::package_features(kPointer, store.dir, "widget", "1.0.0", kLinux);
    expect(eq(a.state, std::string("unreadable")));
    expect(!a.error.empty());
    const auto j = pm::feature_answer_json(a);
    expect(j["list"].is_null() && j.contains("error")) << j.dump();
}};
