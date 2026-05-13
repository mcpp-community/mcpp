#include <gtest/gtest.h>

import std;
import mcpp.toolchain.registry;

using namespace mcpp::toolchain;

TEST(ToolchainRegistry, MapsGccSpecToGccPackage) {
    auto spec = parse_toolchain_spec("gcc@16.1.0");
    ASSERT_TRUE(spec.has_value()) << spec.error();

    auto pkg = to_xim_package(*spec);
    EXPECT_EQ(pkg.ximName, "gcc");
    EXPECT_EQ(pkg.ximVersion, "16.1.0");
    EXPECT_EQ(pkg.display_spec(), "gcc@16.1.0");
    ASSERT_FALSE(pkg.frontendCandidates.empty());
    EXPECT_EQ(pkg.frontendCandidates.front(), "g++");
    EXPECT_TRUE(pkg.needsGccPostInstallFixup);
}

TEST(ToolchainRegistry, MapsGccMuslSuffixToMuslGccPackage) {
    auto spec = parse_toolchain_spec("gcc@15.1.0-musl");
    ASSERT_TRUE(spec.has_value()) << spec.error();

    auto pkg = to_xim_package(*spec);
    EXPECT_TRUE(spec->isMusl);
    EXPECT_EQ(pkg.ximName, "musl-gcc");
    EXPECT_EQ(pkg.ximVersion, "15.1.0");
    EXPECT_EQ(pkg.display_spec(), "gcc@15.1.0-musl");
    ASSERT_FALSE(pkg.frontendCandidates.empty());
    EXPECT_EQ(pkg.frontendCandidates.front(), "x86_64-linux-musl-g++");
    EXPECT_FALSE(pkg.needsGccPostInstallFixup);
}

TEST(ToolchainRegistry, MapsLlvmAndClangAliasesToLlvmPackage) {
    auto llvmSpec = parse_toolchain_spec("llvm", "20.1.7");
    auto clangSpec = parse_toolchain_spec("clang@20.1.7");
    ASSERT_TRUE(llvmSpec.has_value()) << llvmSpec.error();
    ASSERT_TRUE(clangSpec.has_value()) << clangSpec.error();

    auto llvmPkg = to_xim_package(*llvmSpec);
    auto clangPkg = to_xim_package(*clangSpec);

    EXPECT_EQ(llvmPkg.ximName, "llvm");
    EXPECT_EQ(clangPkg.ximName, "llvm");
    EXPECT_EQ(llvmPkg.ximVersion, "20.1.7");
    EXPECT_EQ(clangPkg.ximVersion, "20.1.7");
    EXPECT_EQ(llvmPkg.display_spec(), "llvm@20.1.7");
    EXPECT_EQ(clangPkg.display_spec(), "clang@20.1.7");
    ASSERT_FALSE(clangPkg.frontendCandidates.empty());
    EXPECT_EQ(clangPkg.frontendCandidates.front(), "clang++");
}

TEST(ToolchainRegistry, ResolvesPartialMuslVersionForDisplayAndPackage) {
    auto spec = parse_toolchain_spec("gcc", "15-musl");
    ASSERT_TRUE(spec.has_value()) << spec.error();

    auto resolved = with_resolved_xim_version(*spec, "15.1.0");
    auto pkg = to_xim_package(resolved);

    EXPECT_EQ(resolved.version, "15.1.0-musl");
    EXPECT_EQ(pkg.ximName, "musl-gcc");
    EXPECT_EQ(pkg.ximVersion, "15.1.0");
    EXPECT_EQ(pkg.display_spec(), "gcc@15.1.0-musl");
}
