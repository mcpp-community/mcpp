#include <gtest/gtest.h>

import std;
import mcpp.toolchain.abi;
import mcpp.toolchain.model;

using namespace mcpp::toolchain;

namespace {

Toolchain make_tc(std::string triple, std::string stdlib, CompilerId cc) {
    Toolchain tc;
    tc.targetTriple = std::move(triple);
    tc.stdlibId     = std::move(stdlib);
    tc.compiler     = cc;
    return tc;
}

}  // namespace

// ─── abi_profile: each dimension from its single canonical source ───────────

TEST(AbiProfile, ClangLibcxxOnLinuxGnuIsGlibcAtLibcLevel) {
    // The bug: a clang+libc++ toolchain targeting *-linux-gnu is glibc at the
    // libc level; only its C++ stdlib is libc++. These are independent dims.
    auto p = abi_profile(make_tc("x86_64-unknown-linux-gnu", "libc++", CompilerId::Clang));
    EXPECT_EQ(p.libc, "glibc");
    EXPECT_EQ(p.cxxStdlib, "libc++");
    EXPECT_EQ(p.arch, "x86_64");
    EXPECT_EQ(p.os, "linux");
    EXPECT_EQ(p.cxxAbi, "itanium");
}

TEST(AbiProfile, GccGlibc) {
    auto p = abi_profile(make_tc("x86_64-linux-gnu", "libstdc++", CompilerId::GCC));
    EXPECT_EQ(p.libc, "glibc");
    EXPECT_EQ(p.cxxStdlib, "libstdc++");
}

TEST(AbiProfile, MuslTripleIsMusl) {
    auto p = abi_profile(make_tc("x86_64-linux-musl", "libstdc++", CompilerId::GCC));
    EXPECT_EQ(p.libc, "musl");
    EXPECT_EQ(p.os, "linux");
}

// ─── abi_check: the regression + the genuine incompatibility ────────────────

TEST(AbiCheck, GlibcCLibraryUnderLibcxxIsCompatible) {
    // Regression lock for the glfw-under-clang false positive.
    auto p = abi_profile(make_tc("x86_64-unknown-linux-gnu", "libc++", CompilerId::Clang));
    auto c = parse_abi_capability("abi:glibc", "compat.glfw");
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE(abi_check(p, {*c}).empty());
}

TEST(AbiCheck, GlibcDepUnderMuslMismatchesOnLibcDim) {
    auto p = abi_profile(make_tc("x86_64-linux-musl", "libstdc++", CompilerId::GCC));
    auto c = parse_abi_capability("abi:glibc", "compat.glfw");
    ASSERT_TRUE(c.has_value());
    auto mm = abi_check(p, {*c});
    ASSERT_EQ(mm.size(), 1u);
    EXPECT_EQ(mm[0].dim, AbiDim::Libc);
    EXPECT_EQ(mm[0].need, "glibc");
    EXPECT_EQ(mm[0].got, "musl");
}

TEST(AbiCheck, UnspecifiedDimensionIsDontCare) {
    // A cxxstdlib constraint must NOT be implied by a bare libc capability.
    auto p = abi_profile(make_tc("x86_64-unknown-linux-gnu", "libc++", CompilerId::Clang));
    EXPECT_TRUE(abi_check(p, {{AbiDim::Arch, "x86_64", "x"}}).empty());      // matches
    EXPECT_FALSE(abi_check(p, {{AbiDim::Arch, "aarch64", "x"}}).empty());    // mismatches
}

// ─── parse_abi_capability: legacy + dimensional forms ───────────────────────

TEST(AbiCapability, LegacyBareFormIsLibc) {
    auto c = parse_abi_capability("abi:glibc", "pkg");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->dim, AbiDim::Libc);
    EXPECT_EQ(c->value, "glibc");
    EXPECT_EQ(c->source, "pkg");
}

TEST(AbiCapability, DimensionalForm) {
    auto c = parse_abi_capability("abi:cxxstdlib=libc++");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->dim, AbiDim::CxxStdlib);
    EXPECT_EQ(c->value, "libc++");
}

TEST(AbiCapability, NonAbiCapabilityIgnored) {
    EXPECT_FALSE(parse_abi_capability("x11.display").has_value());
    EXPECT_FALSE(parse_abi_capability("opengl.glx.driver").has_value());
}

TEST(AbiCapability, UnknownDimensionIgnored) {
    EXPECT_FALSE(parse_abi_capability("abi:nonsense=1").has_value());
}
