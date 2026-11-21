// A descriptor is read through the blocks the target's row names, in order
// (design 2026-10-29 §5.1; #851): the first key with a block wins, and the
// build machine takes no part. Both readers -- the version table under `xpm`
// and the per-platform table of the `mcpp` segment -- follow the same order.


import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.toolchain.platform;

using namespace mcpplibs::mtest;

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
    sources = { "src/*.cpp" },
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
    sources = { "src/*.cpp" },
    cxxflags = { "-DCOMMON" },
    linux = { cxxflags = { "-pthread" } },
}
)";

std::vector<std::string> versions(std::string_view lua, std::string_view os) {
    return mcpp::manifest::list_xpkg_versions(lua, TargetPlatform::for_os(os));
}

} // namespace

UTest a_web_build_reads_its_own_block_before_linux{"DescriptorKeys/AWebBuildReadsItsOwnBlockBeforeLinux", [] {
    expect(eq(versions(kBoth, "emscripten"), (std::vector<std::string>{"4.0.0"})));
    expect(eq(versions(kBoth, "linux"), (std::vector<std::string>{"4.1.0"})));
}};

UTest a_web_build_falls_to_linux_only_by_its_rows_order_not_the_host{"DescriptorKeys/AWebBuildFallsToLinuxOnlyByItsRowsOrderNotTheHost", [] {
    expect(eq(versions(kLinuxOnly, "emscripten"), (std::vector<std::string>{"4.1.0"})));
    expect(eq(versions(kLinuxOnly, "openkal"), (std::vector<std::string>{"4.1.0"})));
}};

UTest a_bare_metal_build_reads_no_hosted_block{"DescriptorKeys/ABareMetalBuildReadsNoHostedBlock", [] {
    expect(versions(kLinuxOnly, "none").empty());
    expect(versions(kLinuxOnly, "windows").empty());
}};

UTest the_segment_table_follows_the_same_order{"DescriptorKeys/TheSegmentTableFollowsTheSameOrder", [] {
    auto web = mcpp::manifest::synthesize_from_xpkg_lua(kBoth, "taskflow", "4.0.0",
                                                        TargetPlatform::for_os("emscripten"));
    if (auto&& failed = expect(fatal(bool(web))); !failed) failed << web.error().format();
    auto const& f = web->buildConfig.cxxflags;
    expect(ne(std::ranges::find(f, "-DWEB"), f.end()));
    expect(eq(std::ranges::find(f, "-pthread"), f.end()));

    auto ok = mcpp::manifest::synthesize_from_xpkg_lua(kLinuxOnly, "taskflow", "4.1.0",
                                                       TargetPlatform::for_os("openkal"));
    if (auto&& failed = expect(fatal(bool(ok))); !failed) failed << ok.error().format();
    expect(ne(std::ranges::find(ok->buildConfig.cxxflags, "-pthread"), ok->buildConfig.cxxflags.end()));
}};

// THE PACKAGES A PAYLOAD RUNS WITH (design 2026-11-21, offline and toolchain defaults §3.5): the
// `runtime` list of a split `deps` table, else the flat list; a commented-out entry, a `build`
// list and a version's own table are not read.
UTest a_payloads_runtime_deps_are_its_declared_companions{"DescriptorKeys/RuntimeDepsAreTheDeclaredCompanions", [] {
    constexpr std::string_view split = R"(
package = {
    name = "gcc",
    xpm = {
        linux = {
            deps = {
                build = { "xim:patchelf@0.18.0" },
                runtime = {
                "xim:glibc@>=2.39", "xim:binutils@2.42.1",
                -- "xim:binutils@2.40",
                "xim:linux-headers@5.11.1",
                },
            },
            ["16.1.0"] = { url = "https://example.com/gcc.tar.gz", sha256 = "ab" },
        },
        windows = { deps = { "xim:mingw-runtime@1" }, ["16.1.0"] = { url = "x" } },
    },
}
)";
    constexpr std::string_view flat = R"(
package = {
    name = "llvm",
    xpm = { linux = { deps = { "xim:glibc@>=2.39", "xim:linux-headers@5.11.1" }, ["23.1.3"] = {} } },
}
)";
    expect(eq(mcpp::manifest::xpkg_runtime_deps(split, TargetPlatform::for_os("linux")),
              (std::vector<std::string>{"xim:glibc@>=2.39", "xim:binutils@2.42.1", "xim:linux-headers@5.11.1"})));
    expect(eq(mcpp::manifest::xpkg_runtime_deps(split, TargetPlatform::for_os("windows")),
              (std::vector<std::string>{"xim:mingw-runtime@1"})));
    expect(eq(mcpp::manifest::xpkg_runtime_deps(flat, TargetPlatform::for_os("linux")),
              (std::vector<std::string>{"xim:glibc@>=2.39", "xim:linux-headers@5.11.1"})));
    expect(mcpp::manifest::xpkg_runtime_deps(flat, TargetPlatform::for_os("macosx")).empty());
}};
