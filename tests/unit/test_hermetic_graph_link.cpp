// #696 (W2c) -- on an isolated graph link, the hermetic check holds every `-L`
// to the store, the build directory and the graph's own package roots.
//
// The check dry-runs the driver (`-###`) and reads the linker invocation it
// prints. A driver stands in here: a script under a path that names `xpkgs`
// (the check covers sandbox toolchains only) printing one linker line with the
// search directories each case needs. The check itself is Linux-only.

#include <gtest/gtest.h>

import std;
import mcpp.build.hermetic;
import mcpp.platform;
import mcpp.toolchain.model;

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_hermetic_graph_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// A driver that answers `-###` with one linker line carrying `searchDirs`.
mcpp::toolchain::Toolchain fake_driver(const std::filesystem::path& root,
                                       const std::vector<std::string>& searchDirs) {
    const auto bin = root / "xpkgs" / "fake-llvm" / "bin";
    std::filesystem::create_directories(bin);
    const auto driver = bin / "clang++";
    std::string line = " \"/usr/bin/ld.lld\" \"-static\" \"-o\" \"/dev/null\"";
    for (auto const& d : searchDirs) line += std::format(" \"-L{}\"", d);
    line += " \"/tmp/nothing.o\"";
    {
        std::ofstream os(driver);
        os << "#!/bin/sh\ncat <<'EOF'\n" << line << "\nEOF\n";
    }
    std::filesystem::permissions(driver, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);
    mcpp::toolchain::Toolchain tc;
    tc.compiler     = mcpp::toolchain::CompilerId::Clang;
    tc.binaryPath   = driver;
    tc.targetTriple = "x86_64-linux-musl";
    return tc;
}

}  // namespace

TEST(HermeticGraphLink, AHostSearchDirectoryIsRefusedAndNamed) {
    if constexpr (!mcpp::platform::is_linux) GTEST_SKIP() << "the check runs on Linux hosts";
    Tmp t;
    auto tc = fake_driver(t.path, {"/usr/lib", (t.path / "xpkgs" / "musl" / "lib").string()});
    auto r = mcpp::build::verify_hermetic_link(tc, "", t.path / "out", false,
                                               /*isolatedGraphLink=*/true, {});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("/usr/lib"), std::string::npos) << r.error();
    EXPECT_NE(r.error().find("mcpp#696"), std::string::npos) << r.error();
    EXPECT_EQ(r.error().find("xpkgs/musl"), std::string::npos)
        << "a store directory is allowed and must not be named: " << r.error();
}

TEST(HermeticGraphLink, AllowHostLibsDowngradesTheRefusal) {
    if constexpr (!mcpp::platform::is_linux) GTEST_SKIP() << "the check runs on Linux hosts";
    Tmp t;
    auto tc = fake_driver(t.path, {"/usr/lib"});
    auto r = mcpp::build::verify_hermetic_link(tc, "", t.path / "out", true,
                                               /*isolatedGraphLink=*/true, {});
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
}

// The build directory holds the empty graph sysroot, and a package may add a
// package-relative `-L` whose root is anywhere (a path dependency).
TEST(HermeticGraphLink, TheBuildAndTheGraphsPackagesAreAllowed) {
    if constexpr (!mcpp::platform::is_linux) GTEST_SKIP() << "the check runs on Linux hosts";
    Tmp t;
    const auto out = t.path / "out";
    const auto pathDep = t.path / "elsewhere" / "openkal-musl";
    auto tc = fake_driver(t.path, {(out / "graph-sysroot" / "lib").string(),
                                   (pathDep / "lib" / "empty").string(),
                                   (t.path / "xpkgs" / "musl" / "lib").string()});
    auto r = mcpp::build::verify_hermetic_link(tc, "", out, false,
                                               /*isolatedGraphLink=*/true, {pathDep});
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
}

// A payload link keeps its payload's directories; the `-L` rule is for the
// isolated graph link only.
TEST(HermeticGraphLink, APayloadLinkIsNotHeldToTheGraphRule) {
    if constexpr (!mcpp::platform::is_linux) GTEST_SKIP() << "the check runs on Linux hosts";
    Tmp t;
    auto tc = fake_driver(t.path, {"/usr/lib"});
    auto r = mcpp::build::verify_hermetic_link(tc, "", t.path / "out", false,
                                               /*isolatedGraphLink=*/false, {});
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
}
