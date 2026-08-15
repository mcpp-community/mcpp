#include <gtest/gtest.h>

import std;
import mcpp.toolchain.model;
import mcpp.toolchain.msvc;
import mcpp.toolchain.registry;
import mcpp.toolchain.dialect;

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
    // The persisted default is always the stable "msvc@system" — it matches
    // whatever concrete version detection reports (family-level match).
    auto def = parse_toolchain_spec("msvc@system");
    ASSERT_TRUE(def.has_value());
    PayloadIdentity msvcId{ Family::Msvc, {} };
    EXPECT_TRUE(spec_matches_payload(*def, msvcId, "19.44.35211"));
    EXPECT_TRUE(spec_matches_payload(*def, msvcId, "19.29.30133"));
    PayloadIdentity gccId{ Family::Gcc, {} };
    EXPECT_FALSE(spec_matches_payload(*def, gccId, "16.1.0"));
    auto gccDef = parse_toolchain_spec("gcc@16.1.0");
    ASSERT_TRUE(gccDef.has_value());
    EXPECT_FALSE(spec_matches_payload(*gccDef, msvcId, "19.44.35211"));
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

// ─── std module minimum standard level ───────────────────────────────────
//
// microsoft/STL#3945 ("Supporting `import std;` in C++20") was fixed by
// STL#3977, first shipping in VS 2022 17.8 = cl 19.38. Older STLs still block
// C++20 and would fail inside std.ixx; they answer 23 so the build layer can
// refuse with an actionable message instead.
// See .agents/docs/2026-07-31-cpp20-standard-support-design.md §2.3.

TEST(MsvcStdModule, MinLevelFollowsStlUnblockVersion) {
    auto tc_of = [](std::string ver) {
        Toolchain tc;
        tc.compiler = CompilerId::MSVC;
        tc.version = std::move(ver);
        return tc;
    };
    // VS 2022 17.8 and newer: C++20 is allowed.
    EXPECT_EQ(msvc::std_module_min_level(tc_of("19.38.33130")), 20);
    EXPECT_EQ(msvc::std_module_min_level(tc_of("19.44.35211")), 20);
    EXPECT_EQ(msvc::std_module_min_level(tc_of("20.0")), 20);
    // Older toolsets keep the C++23 floor.
    EXPECT_EQ(msvc::std_module_min_level(tc_of("19.37.32825")), 23);
    EXPECT_EQ(msvc::std_module_min_level(tc_of("19.29.30153")), 23);
    // Unparseable banner version: stay strict rather than guess.
    EXPECT_EQ(msvc::std_module_min_level(tc_of("")), 23);
    EXPECT_EQ(msvc::std_module_min_level(tc_of("unknown")), 23);
}

// #422 — the std module must be built with the SAME CRT model as the TUs that
// import it.
//
// cl bakes `_MSVC_MT` / `_MSVC_MD` into every module it produces. The std build
// passed no `/M` flag at all, so it took cl's default (`/MT`), while a project
// on the default (dynamic) linkage compiles `/MD`. cl accepts the mismatch with
// a C5050 warning and then fails for real inside the ucrt headers:
//
//     corecrt_malloc.h(89): error C2375: 'free': redefinition; different linkage
//
// Two properties are pinned here, and the second is the one that makes the
// first stay true: the flag must be IN the command (so the module is right),
// and it must be in the command STRING (so it enters `std_build_commands`,
// which is part of the std cache identity — two CRT models then cannot share a
// cache directory and silently serve each other's module).
TEST(ToolchainMsvc, StdModuleCarriesTheProjectCrtModel) {
    Toolchain tc;
    tc.compiler   = CompilerId::MSVC;
    tc.binaryPath = "C:/vc/bin/cl.exe";
    tc.stdModuleSource  = "C:/vc/modules/std.ixx";
    tc.stdCompatSource  = "C:/vc/modules/std.compat.ixx";

    const std::filesystem::path cache = "C:/cache/std/key";

    for (std::string_view crt : {std::string_view("/MT"), std::string_view("/MD")}) {
        auto cmds = msvc::std_module_build_commands(tc, cache, "/std:c++23", crt);
        ASSERT_EQ(cmds.size(), 1u);
        EXPECT_NE(cmds[0].find(crt), std::string::npos)
            << crt << " missing from the std build command: " << cmds[0];

        auto compat = msvc::std_compat_build_commands(tc, cache, "/std:c++23", crt);
        ASSERT_EQ(compat.size(), 1u);
        EXPECT_NE(compat[0].find(crt), std::string::npos)
            << crt << " missing from the std.compat build command: " << compat[0];
    }

    // The two models must produce DIFFERENT command strings. Equal strings would
    // mean equal cache keys, i.e. the exact silent divergence this fixes.
    EXPECT_NE(msvc::std_module_build_commands(tc, cache, "/std:c++23", "/MT")[0],
              msvc::std_module_build_commands(tc, cache, "/std:c++23", "/MD")[0]);

    // Empty keeps cl's own default, so non-MSVC callers are unaffected.
    auto bare = msvc::std_module_build_commands(tc, cache, "/std:c++23");
    EXPECT_EQ(bare[0].find("/MT"), std::string::npos);
    EXPECT_EQ(bare[0].find("/MD"), std::string::npos);
}

// The mapping linkage -> CRT model lives in ONE place. It was derived twice —
// flags.cppm for the project's TUs, and cl's default for the std module — which
// is how they disagreed.
TEST(ToolchainMsvc, CrtFlagHasASingleDerivation) {
    Toolchain cl;
    cl.compiler = CompilerId::MSVC;
    const auto& msvcDialect = dialect_for(cl);
    EXPECT_EQ(msvc_crt_flag(msvcDialect, /*staticLinkage=*/true),  "/MT");
    EXPECT_EQ(msvc_crt_flag(msvcDialect, /*staticLinkage=*/false), "/MD");

    // GNU has no counterpart; the helper must yield nothing rather than invent
    // a flag that would be passed to gcc.
    Toolchain gcc;
    gcc.compiler = CompilerId::GCC;
    const auto& gnu = dialect_for(gcc);
    EXPECT_TRUE(msvc_crt_flag(gnu, false).empty());
}
