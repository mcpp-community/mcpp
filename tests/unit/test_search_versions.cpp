// versions_for_hit — the per-hit enrichment behind `mcpp search` (#487).
// A hit's `ns:name` must route to its descriptor and yield the merged
// semver-descending version union; anything unreadable degrades to empty
// without failing the search.


import std;
import mcpplibs.mtest;
import mcpp.config;
import mcpp.pm.package_fetcher;

using namespace mcpplibs::mtest;

namespace {

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / std::format("mcpp-search-versions-{}",
                      std::chrono::steady_clock::now()
                          .time_since_epoch().count());

    TempDir() { std::filesystem::create_directories(path); }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

mcpp::pm::Fetcher::SearchHit hit(std::string name) {
    return {"mcpplibs", std::move(name), "description"};
}

UTest merges_per_os_tables_of_the_hits_descriptor{"SearchVersions/MergesPerOsTablesOfTheHitsDescriptor", [] {
    TempDir temp;
    // Layout rule: the reader derives the letter dir from the candidate
    // FILENAME's first char — "compat.gadget.lua" lives under pkgs/c/.
    auto pkgs = temp.path / "data" / "mcpplibs" / "pkgs" / "c";
    std::filesystem::create_directories(pkgs);
    std::ofstream out(pkgs / "compat.gadget.lua");
    out << R"(package = {
    spec = "1", namespace = "compat", name = "gadget",
    xpm = { linux   = { ["1.0.0"] = { url = "u", sha256 = "h" },
                        ["2.0.0"] = { url = "u", sha256 = "h" } },
            windows = { ["2.0.0"] = { url = "u", sha256 = "h" } } },
}
)";
    out.close();   // the reader runs while this test body is still alive

    mcpp::config::GlobalConfig cfg{};
    cfg.registryDir = temp.path;

    mcpp::pm::Fetcher f(cfg);
    auto versions = f.versions_for_hit(hit("compat:gadget"));
    expect(fatal(eq(versions.size(), 2u)));
    expect(eq(versions[0], "2.0.0"));
    expect(eq(versions[1], "1.0.0"));
}};

UTest bare_name_without_namespace_carries_no_versions{"SearchVersions/BareNameWithoutNamespaceCarriesNoVersions", [] {
    mcpp::config::GlobalConfig cfg{};
    cfg.registryDir = std::filesystem::temp_directory_path()
        / "mcpp-search-versions-no-registry";
    mcpp::pm::Fetcher f(cfg);
    expect(f.versions_for_hit(hit("gadget")).empty());
}};

UTest unreadable_descriptor_degrades_to_empty{"SearchVersions/UnreadableDescriptorDegradesToEmpty", [] {
    TempDir temp;
    mcpp::config::GlobalConfig cfg{};
    cfg.registryDir = temp.path;   // data/ exists nowhere: nothing readable

    mcpp::pm::Fetcher f(cfg);
    expect(f.versions_for_hit(hit("compat:ghost")).empty());
}};

} // namespace
