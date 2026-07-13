#include <gtest/gtest.h>

import std;
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

TEST(MingwSpec, MapsToMingwGccPackage) {
    auto spec = parse_toolchain_spec("mingw@16.1.0");
    ASSERT_TRUE(spec.has_value());
    EXPECT_FALSE(is_system_toolchain(*spec));
    auto pkg = to_xim_package(*spec);
    EXPECT_EQ(pkg.ximName, "mingw-gcc");
    EXPECT_EQ(pkg.ximVersion, "16.1.0");
    EXPECT_EQ(pkg.display_spec(), "mingw@16.1.0");
    ASSERT_FALSE(pkg.frontendCandidates.empty());
    EXPECT_EQ(pkg.frontendCandidates.front(), "g++.exe");
    EXPECT_FALSE(pkg.needsGccPostInstallFixup);
}

TEST(MingwSpec, DisplayAndDefaultMatching) {
    EXPECT_EQ(display_label("mingw-gcc", "16.1.0"), "mingw 16.1.0");
    EXPECT_TRUE(matches_default_toolchain("mingw@16.1.0", "mingw-gcc", "16.1.0"));
    EXPECT_FALSE(matches_default_toolchain("mingw@16.1.0", "mingw-gcc", "15.1.0"));
    EXPECT_FALSE(matches_default_toolchain("gcc@16.1.0", "mingw-gcc", "16.1.0"));
}

TEST(MingwModel, TargetPredicate) {
    EXPECT_TRUE(is_mingw_target(make_tc(CompilerId::GCC, "x86_64-w64-mingw32")));
    EXPECT_FALSE(is_mingw_target(make_tc(CompilerId::GCC, "x86_64-linux-gnu")));
    EXPECT_FALSE(is_mingw_target(make_tc(CompilerId::Clang, "x86_64-pc-windows-msvc")));
}
