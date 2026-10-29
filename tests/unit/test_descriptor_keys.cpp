// A descriptor is read through the blocks the target's row names, in order
// (design 2026-10-29 §5.1; #851): the first key with a block wins, and the
// build machine takes no part. Both readers -- the version table under `xpm`
// and the per-platform table of the `mcpp` segment -- follow the same order.

#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.toolchain.platform;

namespace {

using mcpp::toolchain::platform::TargetPlatform;

constexpr std::string_view kBoth = R"(
package = {
    name = "taskflow",
    xpm = {
        linux = { ["4.1.0"] = { url = "https://example.org/linux.tar.gz" } },
        emscripten = { ["4.0.0"] = { url = "https://example.org/web.tar.gz" } },
    },
}
mcpp = {
    cxxflags = { "-DCOMMON" },
    linux = { cxxflags = { "-pthread" } },
    emscripten = { cxxflags = { "-DWEB" } },
}
)";

constexpr std::string_view kLinuxOnly = R"(
package = {
    name = "taskflow",
    xpm = { linux = { ["4.1.0"] = { url = "https://example.org/linux.tar.gz" } } },
}
mcpp = {
    cxxflags = { "-DCOMMON" },
    linux = { cxxflags = { "-pthread" } },
}
)";

std::vector<std::string> versions(std::string_view lua, std::string_view os) {
    return mcpp::manifest::list_xpkg_versions(lua, TargetPlatform::for_os(os));
}

} // namespace

TEST(DescriptorKeys, AWebBuildReadsItsOwnBlockBeforeLinux) {
    EXPECT_EQ(versions(kBoth, "emscripten"), (std::vector<std::string>{"4.0.0"}));
    EXPECT_EQ(versions(kBoth, "linux"), (std::vector<std::string>{"4.1.0"}));
}

TEST(DescriptorKeys, AWebBuildFallsToLinuxOnlyByItsRowsOrderNotTheHost) {
    EXPECT_EQ(versions(kLinuxOnly, "emscripten"), (std::vector<std::string>{"4.1.0"}));
    EXPECT_EQ(versions(kLinuxOnly, "openkal"), (std::vector<std::string>{"4.1.0"}));
}

TEST(DescriptorKeys, ABareMetalBuildReadsNoHostedBlock) {
    EXPECT_TRUE(versions(kLinuxOnly, "none").empty());
    EXPECT_TRUE(versions(kLinuxOnly, "windows").empty());
}

TEST(DescriptorKeys, TheSegmentTableFollowsTheSameOrder) {
    auto web = mcpp::manifest::synthesize_from_xpkg_lua(kBoth, "taskflow", "4.0.0",
                                                        TargetPlatform::for_os("emscripten"));
    ASSERT_TRUE(web) << web.error().format();
    auto const& f = web->buildConfig.cxxflags;
    EXPECT_NE(std::ranges::find(f, "-DWEB"), f.end());
    EXPECT_EQ(std::ranges::find(f, "-pthread"), f.end());

    auto ok = mcpp::manifest::synthesize_from_xpkg_lua(kLinuxOnly, "taskflow", "4.1.0",
                                                       TargetPlatform::for_os("openkal"));
    ASSERT_TRUE(ok) << ok.error().format();
    EXPECT_NE(std::ranges::find(ok->buildConfig.cxxflags, "-pthread"), ok->buildConfig.cxxflags.end());
}
