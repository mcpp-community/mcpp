#include <gtest/gtest.h>

import std;
import mcpp.xlings;

namespace {

std::filesystem::path make_tempdir(std::string_view name) {
    auto base = std::filesystem::temp_directory_path()
              / std::format("{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(base);
    return base;
}

}  // namespace

TEST(XlingsIndexFreshness, RequiresDefaultMcpplibsIndex) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs");

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, AcceptsFreshDefaultMcpplibsIndex) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");

    mcpp::xlings::Env env{.home = home};

    EXPECT_TRUE(mcpp::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}
