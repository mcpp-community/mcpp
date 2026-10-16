// Fetcher::install_path -- where an installed package of mcpp's indices is
// (design 2026-10-16 §5): the store of the index that serves it, then the
// provider home's, which holds what was installed before that index had a
// store of its own.

#include <gtest/gtest.h>

import std;
import mcpp.config;
import mcpp.pm.package_fetcher;

namespace {

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / std::format("mcpp-install-path-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// A home whose mcpplibs index keeps its payloads in <home>/xpkgs.
mcpp::config::GlobalConfig home_at(const std::filesystem::path& home) {
    mcpp::config::GlobalConfig cfg{};
    cfg.mcppHome = home;
    cfg.registryDir = home / "registry";
    cfg.defaultIndex = "mcpplibs";
    std::filesystem::create_directories(cfg.registryDir);
    std::ofstream(cfg.registryDir / ".xlings.json") << std::format(
        R"({{"index_repos":[{{"name":"mcpplibs","url":"u","dir":"{}","store":"{}"}}]}})",
        (home / "indices" / "mcpplibs").generic_string(), (home / "xpkgs").generic_string());
    return cfg;
}

TEST(FetcherInstallPath, APackageIsFoundInItsIndexStore) {
    TempDir temp;
    auto cfg = home_at(temp.path);
    const auto verdir = temp.path / "xpkgs" / "compat-x-gadget" / "1.0.0";
    std::filesystem::create_directories(verdir);
    mcpp::pm::Fetcher f(cfg);
    const auto found = f.install_path("compat", "gadget", "1.0.0");
    ASSERT_TRUE(found);
    EXPECT_EQ(found->lexically_normal(), verdir.lexically_normal());
}

TEST(FetcherInstallPath, APackageFromBeforeIsFoundInTheHomesStore) {
    TempDir temp;
    auto cfg = home_at(temp.path);
    const auto verdir = cfg.registryDir / "data" / "xpkgs" / "compat-x-gadget" / "1.0.0";
    std::filesystem::create_directories(verdir);
    mcpp::pm::Fetcher f(cfg);
    const auto found = f.install_path("compat", "gadget", "1.0.0");
    ASSERT_TRUE(found);
    EXPECT_EQ(found->lexically_normal(), verdir.lexically_normal());
    EXPECT_FALSE(f.install_path("compat", "gadget", "2.0.0"));
}

} // namespace
