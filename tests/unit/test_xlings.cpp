#include <gtest/gtest.h>
#include <cstdlib>

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
    std::ofstream(home / "data" / "mcpplibs" / ".mcpp-index-updated") << "ok\n";

    mcpp::xlings::Env env{.home = home};

    EXPECT_TRUE(mcpp::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, RequiresRefreshMarkerForDefaultMcpplibsIndex) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, RejectsStaleRefreshMarker) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");
    auto marker = home / "data" / "mcpplibs" / ".mcpp-index-updated";
    std::ofstream(marker) << "ok\n";
    std::filesystem::last_write_time(
        marker, std::filesystem::file_time_type::clock::now() - std::chrono::seconds(7200));

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, RequiresOfficialXimIndexEvenWhenDefaultIndexIsFresh) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");
    std::ofstream(home / "data" / "mcpplibs" / ".mcpp-index-updated") << "ok\n";

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_official_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, AcceptsFreshOfficialXimIndex) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs");
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";

    mcpp::xlings::Env env{.home = home};

    EXPECT_TRUE(mcpp::xlings::is_official_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, RequiresOfficialPackageFileEvenWhenOfficialIndexIsFresh) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs");
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, AcceptsFreshOfficialPackageFile) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs" / "m");
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";
    std::ofstream(home / "data" / "xim-pkgindex" / "pkgs" / "m" / "musl-gcc.lua") << "package = {}\n";

    mcpp::xlings::Env env{.home = home};

    EXPECT_TRUE(mcpp::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, RejectsOfficialPackageCacheWithForeignPath) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    auto pkg = home / "data" / "xim-pkgindex" / "pkgs" / "m" / "musl-gcc.lua";
    std::filesystem::create_directories(pkg.parent_path());
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";
    std::ofstream(pkg) << "package = {}\n";
    std::ofstream(home / "data" / "xim-pkgindex" / ".xlings-index-cache.json")
        << R"({"entries":{"musl-gcc":{"path":"/tmp/foreign/xim-pkgindex/pkgs/m/musl-gcc.lua"}}})";

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, AcceptsOfficialPackageCacheWithCurrentPath) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    auto pkg = home / "data" / "xim-pkgindex" / "pkgs" / "m" / "musl-gcc.lua";
    std::filesystem::create_directories(pkg.parent_path());
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";
    std::ofstream(pkg) << "package = {}\n";
    std::ofstream(home / "data" / "xim-pkgindex" / ".xlings-index-cache.json")
        << std::format(R"({{"entries":{{"musl-gcc":{{"path":"{}"}}}}}})", pkg.string());

    mcpp::xlings::Env env{.home = home};

    EXPECT_TRUE(mcpp::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}

// ─── Sibling/home payload discovery (issue #120) ─────────────────────
//
// A delegating index package (e.g. xim:linux-headers forwarding to
// scode:linux-headers) leaves a metadata-only husk dir under its own
// prefix (.xim-installed + .xpkg.lua, no payload). Discovery must not
// stop at the husk: the real payload lives under another prefix.

namespace {

void touch(const std::filesystem::path& p, std::string_view content = "x") {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

}  // namespace

TEST(XlingsSiblingPackage, MetadataOnlyHuskIsNotContent) {
    auto tmp = make_tempdir("mcpp-husk");
    auto xpkgs = tmp / "xpkgs";
    auto gccBin = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
    touch(gccBin);

    // Only a husk exists: .xim-installed + .xpkg.lua, no payload.
    auto husk = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(husk / ".xim-installed");
    touch(husk / ".xpkg.lua", "package = {}");

    // Isolate from the host's ~/.xlings fallback.
    const char* oldHome = std::getenv("HOME");
    ::setenv("HOME", tmp.c_str(), 1);
    auto found = mcpp::xlings::paths::find_sibling_package(gccBin, "linux-headers");
    if (oldHome) ::setenv("HOME", oldHome, 1); else ::unsetenv("HOME");

    EXPECT_FALSE(found.has_value());

    std::filesystem::remove_all(tmp);
}

TEST(XlingsSiblingPackage, SkipsHuskAndFindsPayloadUnderOtherPrefix) {
    auto tmp = make_tempdir("mcpp-husk");
    auto xpkgs = tmp / "xpkgs";
    auto gccBin = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
    touch(gccBin);

    auto husk = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(husk / ".xim-installed");
    touch(husk / ".xpkg.lua", "package = {}");

    auto real = xpkgs / "scode-x-linux-headers" / "5.11.1";
    touch(real / "include" / "linux" / "limits.h");

    auto found = mcpp::xlings::paths::find_sibling_package(gccBin, "linux-headers");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, real);

    std::filesystem::remove_all(tmp);
}

TEST(XlingsSiblingPackage, RequiredRelPathRejectsContentfulButWrongCandidate) {
    auto tmp = make_tempdir("mcpp-husk");
    auto xpkgs = tmp / "xpkgs";
    auto gccBin = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
    touch(gccBin);

    // Contentful but missing the payload that matters.
    auto stray = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(stray / "README.md");

    auto real = xpkgs / "scode-x-linux-headers" / "5.11.1";
    touch(real / "include" / "linux" / "limits.h");

    auto found = mcpp::xlings::paths::find_sibling_package(
        gccBin, "linux-headers", "include/linux/limits.h");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, real);

    std::filesystem::remove_all(tmp);
}

TEST(XlingsHomeTool, FindsPayloadUnderNonXimPrefix) {
    auto tmp = make_tempdir("mcpp-husk-home");
    auto xpkgs = tmp / "registry" / "data" / "xpkgs";

    auto husk = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(husk / ".xim-installed");
    touch(husk / ".xpkg.lua", "package = {}");

    auto real = xpkgs / "scode-x-linux-headers" / "5.11.1";
    touch(real / "include" / "linux" / "limits.h");

    ::setenv("MCPP_HOME", tmp.c_str(), 1);
    auto found = mcpp::xlings::paths::find_home_tool(
        "linux-headers", "include/linux/limits.h");
    ::unsetenv("MCPP_HOME");

    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, real);

    std::filesystem::remove_all(tmp);
}
