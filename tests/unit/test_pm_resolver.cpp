import std;
import mcpplibs.mtest;
import mcpp.pm.index_route;
import mcpp.pm.index_spec;
import mcpp.pm.resolver;
import mcpp.toolchain.platform;

using namespace mcpplibs::mtest;

namespace {

// One package of a local index, `acme.<name>`, publishing `keys` for every host.
struct OnePackage {
    std::filesystem::path root;
    mcpp::pm::IndexMap    indices;

    OnePackage(std::string_view name, std::initializer_list<std::string_view> keys) {
        root = std::filesystem::temp_directory_path() / std::format("mcpp-resolver-{}", name);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "pkgs" / "a");
        std::string table = "{\n";
        for (auto key : keys) table += std::format("            [\"{}\"] = {{ url = \"u\", sha256 = \"0\" }},\n", key);
        table += "        }";
        std::ofstream(root / "pkgs" / "a" / std::format("acme.{}.lua", name)) << std::format(
            "package = {{\n    spec = \"1\", namespace = \"acme\", name = \"{0}\", type = \"package\",\n"
            "    xpm = {{\n        linux = {1},\n        macosx = {1},\n        windows = {1},\n    }},\n}}\n", name, table);
        indices["acme"] = mcpp::pm::IndexSpec{ .name = "acme", .path = root };
    }
    ~OnePackage() { std::error_code ec; std::filesystem::remove_all(root, ec); }

    std::expected<std::string, std::string> resolve(std::string_view name, std::string_view constraint) {
        mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };
        return mcpp::pm::resolve_semver("acme", name, constraint, route,
                                        mcpp::toolchain::platform::HostPlatform::current());
    }
};

bool has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

} // namespace

// mcpp#370: compat.glad's shape. The one key is a pre-release, which no range that
// names a release reaches; the refusal says so and names the pin.
UTest a_range_over_a_pre_release_names_the_pin{"PmResolver/ARangeOverAPreReleaseNamesThePin", [] {
    OnePackage idx("pre", {"0.0.0-651a425"});
    for (std::string_view constraint : {"*", "^0.0", ">=0"}) {
        auto r = idx.resolve("pre", constraint);
        expect(fatal(!r.has_value())) << constraint << " reached a pre-release";
        expect(has(r.error(), "matches none of: [0.0.0-651a425]")) << r.error();
        expect(has(r.error(), "0.0.0-651a425 is a pre-release")) << r.error();
        expect(has(r.error(), "\n  help: pin it exactly: acme.pre = \"0.0.0-651a425\"")) << r.error();
    }
    // The pin the refusal names is an answer.
    auto pinned = idx.resolve("pre", "=0.0.0-651a425");
    expect(fatal(pinned.has_value())) << pinned.error();
    expect(eq(*pinned, "0.0.0-651a425"));
}};

UTest the_pin_named_is_the_highest_pre_release_the_range_admits{"PmResolver/ThePinNamedIsTheHighestPreReleaseTheRangeAdmits", [] {
    OnePackage idx("rc", {"1.0.0-rc.1", "1.0.0-rc.2", "2.0.0-rc.1"});
    auto r = idx.resolve("rc", "^1.0");
    expect(fatal(!r.has_value()));
    expect(has(r.error(), "acme.rc = \"1.0.0-rc.2\"")) << r.error();
    // A release among the keys does not hide the cause: `^2` admits 2.0.0.
    OnePackage mixed("mixed", {"1.0.0", "2.0.0-rc.1"});
    r = mixed.resolve("mixed", "^2");
    expect(fatal(!r.has_value()));
    expect(has(r.error(), "acme.mixed = \"2.0.0-rc.1\"")) << r.error();
}};

// A pre-release whose release the range excludes as well is an ordinary miss: a pin of
// it would answer another question than the one the range asked.
UTest a_pre_release_outside_the_range_is_an_ordinary_miss{"PmResolver/APreReleaseOutsideTheRangeIsAnOrdinaryMiss", [] {
    OnePackage idx("far", {"0.0.0-651a425", "1.2.0"});
    auto r = idx.resolve("far", "^3");
    expect(fatal(!r.has_value()));
    expect(has(r.error(), "matches none of")) << r.error();
    expect(!has(r.error(), "pre-release")) << r.error();
    expect(!has(r.error(), "help:")) << r.error();
    // So is one the range names and excludes: `^1.0.0-rc.3` sees rc.2 and asks for more.
    OnePackage seen("seen", {"1.0.0-rc.1", "1.0.0-rc.2"});
    r = seen.resolve("seen", "^1.0.0-rc.3");
    expect(fatal(!r.has_value()));
    expect(!has(r.error(), "help:")) << r.error();
}};
