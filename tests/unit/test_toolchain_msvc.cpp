#include <gtest/gtest.h>

import std;
import mcpp.toolchain.model;
import mcpp.toolchain.msvc;
import mcpp.toolchain.registry;

using namespace mcpp::toolchain;

// ─── parse_cl_banner ─────────────────────────────────────────────────────

TEST(MsvcBanner, ParsesEnglishBanner) {
    auto r = msvc::parse_cl_banner(
        "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35211 for x64\n"
        "Copyright (C) Microsoft Corporation.  All rights reserved.\n"
        "\n"
        "usage: cl [ option... ] filename... [ /link linkoption... ]\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first, "19.44.35211");
    EXPECT_EQ(r->second, "x64");
}

TEST(MsvcBanner, ParsesLocalizedBannerByTokens) {
    // Chinese VS reorders the sentence; only the tokens are stable.
    auto r = msvc::parse_cl_banner(
        "用于 x64 的 Microsoft (R) C/C++ 优化编译器 19.44.35211 版\n"
        "版权所有(C) Microsoft Corporation。保留所有权利。\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first, "19.44.35211");
    EXPECT_EQ(r->second, "x64");
}

TEST(MsvcBanner, ParsesArm64AndFourComponentVersions) {
    auto r = msvc::parse_cl_banner(
        "Microsoft (R) C/C++ Optimizing Compiler Version 19.29.30133.0 for ARM64\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first, "19.29.30133.0");
    EXPECT_EQ(r->second, "arm64");
}

TEST(MsvcBanner, RejectsGarbage) {
    EXPECT_FALSE(msvc::parse_cl_banner("").has_value());
    EXPECT_FALSE(msvc::parse_cl_banner("bash: cl: command not found").has_value());
    // A bare two-component number is not a cl version.
    EXPECT_FALSE(msvc::parse_cl_banner("something 1.2 else").has_value());
}

TEST(MsvcBanner, TripleForArch) {
    EXPECT_EQ(msvc::triple_for_arch("x64"),   "x86_64-pc-windows-msvc");
    EXPECT_EQ(msvc::triple_for_arch("x86"),   "i686-pc-windows-msvc");
    EXPECT_EQ(msvc::triple_for_arch("arm64"), "aarch64-pc-windows-msvc");
    // Unknown/empty arch falls back to the x64 triple.
    EXPECT_EQ(msvc::triple_for_arch(""),      "x86_64-pc-windows-msvc");
}

// ─── install guidance ────────────────────────────────────────────────────

TEST(MsvcGuidance, MentionsInstallRoutesAndOwnership) {
    auto g = msvc::install_guidance();
    ASSERT_FALSE(g.empty());
    EXPECT_NE(g.find("winget"), std::string::npos);
    EXPECT_NE(g.find("does not install"), std::string::npos);
    EXPECT_NE(g.find("mcpp toolchain default msvc"), std::string::npos);
}

// ─── spec layer ──────────────────────────────────────────────────────────

TEST(MsvcSpec, SystemToolchainClassification) {
    for (auto s : {"msvc", "msvc@system", "msvc@19.44"}) {
        auto spec = parse_toolchain_spec(s);
        ASSERT_TRUE(spec.has_value()) << s;
        EXPECT_TRUE(is_system_toolchain(*spec)) << s;
    }
    auto gcc = parse_toolchain_spec("gcc@16.1.0");
    ASSERT_TRUE(gcc.has_value());
    EXPECT_FALSE(is_system_toolchain(*gcc));
}

TEST(MsvcSpec, StableDefaultMatchesAnyDetectedVersion) {
    EXPECT_TRUE(matches_default_toolchain("msvc@system", "msvc", "19.44.35211"));
    EXPECT_TRUE(matches_default_toolchain("msvc@system", "msvc", "19.29.30133"));
    EXPECT_FALSE(matches_default_toolchain("msvc@system", "gcc", "16.1.0"));
    EXPECT_FALSE(matches_default_toolchain("gcc@16.1.0", "msvc", "19.44.35211"));
}

// ─── model traits ────────────────────────────────────────────────────────

TEST(MsvcModel, BmiTraitsUseIfc) {
    Toolchain tc;
    tc.compiler = CompilerId::MSVC;
    auto t = bmi_traits(tc);
    EXPECT_EQ(t.bmiDir, "ifc.cache");
    EXPECT_EQ(t.bmiExt, ".ifc");
    EXPECT_TRUE(t.needsExplicitModuleOutput);
    EXPECT_FALSE(t.scanNeedsFModules);
}
