#include <gtest/gtest.h>

import std;
import mcpp.pm.package_fetcher;

// Regression for the compat.zlib vs upstream bare zlib.lua collision.
//
// read_xpkg_lua_from_project_data scans every index dir under the project's
// xlings data roots. Before the identity gate, it returned the FIRST file whose
// candidate filename existed — so a foreign `xim-pkgindex/pkgs/z/zlib.lua`
// (declares `name = "zlib"`, no namespace, no mcpp block) could satisfy a
// request for `compat.zlib` whenever it was scanned before the real
// `mcpplibs/pkgs/c/compat.zlib.lua`. Resolution must now key on the descriptor's
// DECLARED identity, independent of directory order.

namespace {

std::filesystem::path make_tempdir(std::string_view name) {
    auto base = std::filesystem::temp_directory_path()
              / std::format("{}-{}", name,
                            std::chrono::steady_clock::now()
                                .time_since_epoch().count());
    std::filesystem::create_directories(base);
    return base;
}

void write_file(const std::filesystem::path& p, std::string_view content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream os(p);
    os << content;
}

constexpr std::string_view kCompatZlibLua = R"(
package = {
    namespace = "compat",
    name      = "compat.zlib",
    version   = "1.3.2",
    mcpp = { sources = { "*.c" } },
}
)";

constexpr std::string_view kUpstreamZlibLua = R"(
package = {
    name = "zlib",
    description = "upstream zlib, no mcpp block",
}
)";

}  // namespace

TEST(PmPackageFetcher, ResolvesCompatZlibNotForeignBareZlib) {
    auto project = make_tempdir("mcpp-fetcher-collision");
    auto dataRoot = project / ".mcpp" / "data";

    // Foreign index sorts BEFORE the owning index, so a naive first-hit scan
    // would return its bare zlib.lua.
    write_file(dataRoot / "aaa-foreign-index" / "pkgs" / "z" / "zlib.lua",
               kUpstreamZlibLua);
    write_file(dataRoot / "mcpplibs" / "pkgs" / "c" / "compat.zlib.lua",
               kCompatZlibLua);

    auto lua = mcpp::pm::Fetcher::read_xpkg_lua_from_project_data(
        project, "compat", "zlib");

    ASSERT_TRUE(lua.has_value());
    EXPECT_NE(lua->find("compat.zlib"), std::string::npos)
        << "should return the compat-namespaced descriptor";
    EXPECT_NE(lua->find("mcpp"), std::string::npos)
        << "the resolved descriptor must carry the mcpp block";

    std::filesystem::remove_all(project);
}

TEST(PmPackageFetcher, ForeignBareZlibAloneDoesNotSatisfyCompatRequest) {
    auto project = make_tempdir("mcpp-fetcher-nomatch");
    auto dataRoot = project / ".mcpp" / "data";

    // Only the foreign bare zlib exists; a compat.zlib request must find nothing
    // rather than wrongly accept the unrelated package.
    write_file(dataRoot / "xim-pkgindex" / "pkgs" / "z" / "zlib.lua",
               kUpstreamZlibLua);

    auto lua = mcpp::pm::Fetcher::read_xpkg_lua_from_project_data(
        project, "compat", "zlib");

    EXPECT_FALSE(lua.has_value());

    std::filesystem::remove_all(project);
}

TEST(PmPackageFetcher, DefaultNamespaceRequestResolvesCompatAliasDescriptor) {
    // The CI break: dev-dep `gtest` (bare → default namespace "mcpplibs") must
    // resolve to the `compat.gtest` descriptor that actually lives in the index.
    auto project = make_tempdir("mcpp-fetcher-compat-alias");
    auto dataRoot = project / ".mcpp" / "data";

    constexpr std::string_view compatGtest = R"(
package = {
    namespace = "compat",
    name      = "compat.gtest",
    version   = "1.15.2",
    mcpp = { sources = { "*.cc" } },
}
)";
    write_file(dataRoot / "mcpplibs" / "pkgs" / "c" / "compat.gtest.lua",
               compatGtest);

    auto lua = mcpp::pm::Fetcher::read_xpkg_lua_from_project_data(
        project, "mcpplibs", "gtest");

    ASSERT_TRUE(lua.has_value()) << "default-ns request must find its compat alias";
    EXPECT_NE(lua->find("compat.gtest"), std::string::npos);

    std::filesystem::remove_all(project);
}

TEST(PmPackageFetcher, LocalPathIndexAttributesOwnNamespaceToNoNsDescriptor) {
    // e2e 49/51: a `[indices]` path index `local-dev` whose descriptor declares
    // only name="tinycfg" (no namespace field). The namespace is owned by the
    // index, so a `(local-dev, tinycfg)` request must match via index-owned
    // namespace attribution.
    auto index = make_tempdir("mcpp-local-path-index");
    write_file(index / "pkgs" / "t" / "tinycfg.lua",
               R"(package = { name = "tinycfg", version = "1.0.0", mcpp = { sources = {"*.c"} } })");

    auto lua = mcpp::pm::Fetcher::read_xpkg_lua_from_path(index, "local-dev", "tinycfg");
    ASSERT_TRUE(lua.has_value()) << "no-namespace descriptor must inherit the index namespace";
    EXPECT_NE(lua->find("tinycfg"), std::string::npos);

    // A short name the index does not contain still resolves to nothing.
    auto miss = mcpp::pm::Fetcher::read_xpkg_lua_from_path(index, "local-dev", "absent");
    EXPECT_FALSE(miss.has_value());

    std::filesystem::remove_all(index);
}

// Coverage gap closed: a custom-namespace descriptor filed under a NON-canonical
// filename must still resolve for a qualified request, because identity is the
// declared (namespace, name) — never the filename (design doc
// 2026-06-26-identity-first-resolution-no-filename.md, P0/P2).
//
// Real-world trigger: `aimol.tensorvia-cpu` declares namespace="aimol",
// name="tensorvia-cpu" but is filed in the mcpplibs index as the BARE
// `pkgs/t/tensorvia-cpu.lua` (canonical would be `pkgs/a/aimol.tensorvia-cpu.lua`).
// Every prior fetcher fixture sat at its canonical path, so this seam was untested.
// `read_xpkg_lua*` already keys on declared identity, so this asserts the READ
// layer is correct and the production failure is isolated to candidate SELECTION
// (`selectDependencyCandidate`'s canonical-filename-only strict reader).
TEST(PmPackageFetcher, ResolvesCustomNamespaceDescriptorUnderNonCanonicalFilename) {
    auto project = make_tempdir("mcpp-noncanonical-filename");
    auto dataRoot = project / ".mcpp" / "data";

    // Declared identity (aimol, tensorvia-cpu), but filed under the bare short
    // name in the mcpplibs index — filename does NOT encode the namespace.
    write_file(dataRoot / "mcpplibs" / "pkgs" / "t" / "tensorvia-cpu.lua",
               R"(package = {
                      namespace = "aimol",
                      name      = "tensorvia-cpu",
                      version   = "0.1.1",
                      mcpp = { sources = { "*.cppm" } },
                  })");

    // Qualified request for the custom namespace must resolve, filename be damned.
    auto hit = mcpp::pm::Fetcher::read_xpkg_lua_from_project_data(
        project, "aimol", "tensorvia-cpu");
    ASSERT_TRUE(hit.has_value())
        << "declared (aimol, tensorvia-cpu) must resolve regardless of filename";
    EXPECT_NE(hit->find("tensorvia-cpu"), std::string::npos);

    // A foreign namespace for the same short name must NOT match it.
    auto wrongNs = mcpp::pm::Fetcher::read_xpkg_lua_from_project_data(
        project, "mcpplibs", "tensorvia-cpu");
    EXPECT_FALSE(wrongNs.has_value())
        << "the descriptor is (aimol, …), so a (mcpplibs, …) request must miss";

    std::filesystem::remove_all(project);
}
