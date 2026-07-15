#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.toolchain.dialect;
import mcpp.toolchain.model;
import mcpp.toolchain.registry;

using namespace mcpp::toolchain;

namespace {
Toolchain make_tc(CompilerId id, std::string triple = {}) {
    Toolchain tc;
    tc.compiler = id;
    tc.targetTriple = std::move(triple);
    return tc;
}
} // namespace

TEST(CommandDialect, GnuSpellings) {
    auto tc = make_tc(CompilerId::GCC);
    const auto& d = dialect_for(tc);
    EXPECT_EQ(d.id, "gnu");
    EXPECT_EQ(d.includePrefix, "-I");
    EXPECT_EQ(d.stdPrefix, "-std=");
    EXPECT_EQ(d.outputObjPrefix, "-o ");
    EXPECT_EQ(d.objExt, ".o");
    EXPECT_EQ(d.archiveCmd, "$ar rcs $out $in");
    EXPECT_FALSE(d.rspfileLink);
    EXPECT_EQ(d.linkStyle, CommandDialect::LinkStyle::Driver);
    // Clang and MinGW share the gnu row.
    EXPECT_EQ(dialect_for(make_tc(CompilerId::Clang)).id, "gnu");
    EXPECT_EQ(dialect_for(make_tc(CompilerId::GCC, "x86_64-w64-mingw32")).id, "gnu");
}

TEST(CommandDialect, MsvcSpellings) {
    const auto& d = dialect_for(make_tc(CompilerId::MSVC));
    EXPECT_EQ(d.id, "msvc");
    EXPECT_EQ(d.includePrefix, "/I");
    EXPECT_EQ(d.stdPrefix, "/std:");
    EXPECT_EQ(d.outputObjPrefix, "/Fo:");
    EXPECT_EQ(d.objExt, ".obj");
    EXPECT_TRUE(d.rspfileLink);
    EXPECT_EQ(d.linkStyle, CommandDialect::LinkStyle::SeparateLinker);
}

TEST(BmiTraitsSpellings, ModuleFlagRowsAreConsistent) {
    auto gcc = bmi_traits(make_tc(CompilerId::GCC));
    EXPECT_EQ(gcc.compileModulesFlag, " -fmodules");
    EXPECT_TRUE(gcc.stdBmiUsePrefix.empty());
    EXPECT_TRUE(gcc.moduleOutputPrefix.empty());

    auto clang = bmi_traits(make_tc(CompilerId::Clang));
    EXPECT_TRUE(clang.compileModulesFlag.empty());
    EXPECT_EQ(clang.stdBmiUsePrefix, " -fmodule-file=std=");
    EXPECT_EQ(clang.moduleOutputPrefix, " -fmodule-output=");
    EXPECT_EQ(clang.bmiSearchPrefix, " -fprebuilt-module-path=");

    auto msvc = bmi_traits(make_tc(CompilerId::MSVC));
    EXPECT_EQ(msvc.stdBmiUsePrefix, " /reference std=");
    EXPECT_EQ(msvc.moduleOutputPrefix, " /ifcOutput ");
    EXPECT_EQ(msvc.bmiSearchPrefix, " /ifcSearchDir ");
}

TEST(MingwSpec, LegacyMingwSpellingIsGccTargetingWindowsGnu) {
    // `mingw` is no longer a family: it normalizes to gcc targeting
    // x86_64-windows-gnu. Which payload serves it is host-split at the
    // distribution layer (winlibs mingw-gcc on Windows, mingw-cross-gcc
    // elsewhere) — the user-facing identity is the same either way.
    auto spec = parse_toolchain_spec("mingw@16.1.0");
    ASSERT_TRUE(spec.has_value());
    EXPECT_FALSE(is_system_toolchain(*spec));
    EXPECT_EQ(spec->family, Family::Gcc);
    EXPECT_EQ(spec->target.str(), "x86_64-windows-gnu");
    EXPECT_EQ(spec->display(), "gcc@16.1.0 → x86_64-windows-gnu");

    auto pkg = to_xim_package(*spec);
    EXPECT_EQ(pkg.ximVersion, "16.1.0");
    ASSERT_FALSE(pkg.frontendCandidates.empty());
#if defined(_WIN32)
    EXPECT_EQ(pkg.ximName, "mingw-gcc");
    EXPECT_EQ(pkg.frontendCandidates.front(), "g++.exe");
#else
    EXPECT_EQ(pkg.ximName, "mingw-cross-gcc");
    EXPECT_EQ(pkg.frontendCandidates.front(), "x86_64-w64-mingw32-g++");
#endif
    EXPECT_FALSE(pkg.needsGccPostInstallFixup);
}

TEST(MingwSpec, PayloadIdentityAndDefaultMatching) {
    // Both host-split payload dirs identify as the SAME (gcc, windows-gnu).
    for (auto dir : {"mingw-gcc", "mingw-cross-gcc"}) {
        auto id = identify_xim_payload(dir);
        ASSERT_TRUE(id.has_value()) << dir;
        EXPECT_EQ(id->family, Family::Gcc) << dir;
        EXPECT_EQ(id->target.str(), "x86_64-windows-gnu") << dir;
    }
    auto id = *identify_xim_payload("mingw-cross-gcc");
    auto def = parse_toolchain_spec("mingw-cross@16.1.0");   // legacy default string
    ASSERT_TRUE(def.has_value());
    EXPECT_TRUE(spec_matches_payload(*def, id, "16.1.0"));
    EXPECT_FALSE(spec_matches_payload(*def, id, "15.1.0"));
    auto llvmDef = parse_toolchain_spec("llvm@16.1.0");
    ASSERT_TRUE(llvmDef.has_value());
    EXPECT_FALSE(spec_matches_payload(*llvmDef, id, "16.1.0"));
}

TEST(StdFlagFor, PerDialectSpelling) {
    const auto& gnu  = dialect_for(make_tc(CompilerId::GCC));
    const auto& msvc = dialect_for(make_tc(CompilerId::MSVC));
    EXPECT_EQ(std_flag_for(gnu, "c++26", 26), "-std=c++26");
    EXPECT_EQ(std_flag_for(gnu, "gnu++23", 23), "-std=gnu++23");
    EXPECT_EQ(std_flag_for(msvc, "c++20", 20), "/std:c++20");
    EXPECT_EQ(std_flag_for(msvc, "c++23", 23), "/std:c++latest");
    EXPECT_EQ(std_flag_for(msvc, "c++26", 26), "/std:c++latest");
}

// ─── issue #210: dialect-class flag extraction ───────────────────────────

TEST(DialectFlags, KnownListAndEscapeHatch) {
    mcpp::manifest::BuildConfig bc;
    bc.cxxflags = {"-freflection", "-O3", "-Wall", "-D_GLIBCXX_USE_CXX11_ABI=0"};
    bc.dialectCxxflags = {"-fcustom-std-thing", "-freflection"};  // dup dedups
    auto flags = mcpp::manifest::dialect_flags(bc);
    ASSERT_EQ(flags.size(), 3u);
    EXPECT_EQ(flags[0], "-fcustom-std-thing");   // explicit first, order kept
    EXPECT_EQ(flags[1], "-freflection");         // deduped against cxxflags copy
    EXPECT_EQ(flags[2], "-D_GLIBCXX_USE_CXX11_ABI=0");  // auto-promoted
}

TEST(DialectFlags, ExtractionDetails) {
    using mcpp::manifest::is_dialect_flag;
    EXPECT_TRUE(is_dialect_flag("-freflection"));
    EXPECT_TRUE(is_dialect_flag("-fcontracts"));
    EXPECT_TRUE(is_dialect_flag("-fno-char8_t"));
    EXPECT_TRUE(is_dialect_flag("-D_GLIBCXX_USE_CXX11_ABI=1"));
    // Conservative first list: these stay per-unit.
    EXPECT_FALSE(is_dialect_flag("-fno-exceptions"));
    EXPECT_FALSE(is_dialect_flag("-fno-rtti"));
    EXPECT_FALSE(is_dialect_flag("-O2"));
    EXPECT_FALSE(is_dialect_flag("-Wall"));
    EXPECT_FALSE(is_dialect_flag("-fPIC"));
}

TEST(MingwModel, TargetPredicate) {
    EXPECT_TRUE(is_mingw_target(make_tc(CompilerId::GCC, "x86_64-w64-mingw32")));
    EXPECT_FALSE(is_mingw_target(make_tc(CompilerId::GCC, "x86_64-linux-gnu")));
    EXPECT_FALSE(is_mingw_target(make_tc(CompilerId::Clang, "x86_64-pc-windows-msvc")));
}
