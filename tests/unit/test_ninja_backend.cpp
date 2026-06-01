#include <gtest/gtest.h>

import std;
import mcpp.build.compile_commands;
import mcpp.build.flags;
import mcpp.build.ninja;
import mcpp.build.plan;
import mcpp.manifest;
import mcpp.toolchain.model;

using namespace mcpp::build;

namespace {

std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

BuildPlan minimal_plan() {
    BuildPlan plan;
    plan.projectRoot = "/tmp/mcpp-ninja-test";
    plan.outputDir = plan.projectRoot / "target" / "test";
    plan.manifest.package.name = "objc_rule_test";
    plan.manifest.buildConfig.cStandard = "c11";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::GCC;
    plan.toolchain.version = "test";
    plan.toolchain.binaryPath = "/usr/bin/g++";
    plan.toolchain.targetTriple = "x86_64-linux-gnu";
    return plan;
}

}  // namespace

TEST(NinjaBackend, ObjectiveCSourceUsesCObjectRuleAndCFlags) {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/cocoa.m",
        .object = "obj/cocoa.o",
        .packageName = "objc_rule_test",
        .packageCflags = {"-DOBJ_C_BUILD=1"},
        .packageCxxflags = {"-DWRONG_CXX_FLAG=1"},
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_NE(ninja.find("build obj/cocoa.o : c_object src/cocoa.m"), std::string::npos)
        << ninja;
    EXPECT_EQ(ninja.find("build obj/cocoa.o : cxx_object src/cocoa.m"), std::string::npos)
        << ninja;
    EXPECT_NE(ninja.find("unit_cflags = -DOBJ_C_BUILD=1"), std::string::npos)
        << ninja;
    EXPECT_EQ(ninja.find("unit_cxxflags = -DWRONG_CXX_FLAG=1"), std::string::npos)
        << ninja;
}

TEST(NinjaBackend, UsesPackageCppStandardForCxxFlags) {
    auto plan = minimal_plan();
    plan.manifest.package.standard = "c++26";
    plan.manifest.language.standard = "c++26";
    plan.cppStandard = "c++26";
    plan.cppStandardFlag = "-std=c++26";
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .object = "obj/main.o",
        .packageName = "cpp26_test",
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_NE(ninja.find("cxxflags  = -std=c++26"), std::string::npos)
        << ninja;
    EXPECT_EQ(ninja.find("-std=c++23"), std::string::npos)
        << ninja;
}

TEST(NinjaBackend, CompileCommandsUsesSameCppStandard) {
    auto plan = minimal_plan();
    plan.manifest.package.standard = "c++26";
    plan.manifest.language.standard = "c++26";
    plan.cppStandard = "c++26";
    plan.cppStandardFlag = "-std=c++26";
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .object = "obj/main.o",
        .packageName = "cpp26_test",
    });

    auto flags = compute_flags(plan);
    auto cdb = emit_compile_commands(plan, flags);

    EXPECT_NE(cdb.find("\"-std=c++26\""), std::string::npos)
        << cdb;
    EXPECT_EQ(cdb.find("\"-std=c++23\""), std::string::npos)
        << cdb;
}

TEST(NinjaBackend, RootPackageCxxflagsAreEmittedOncePerUnit) {
    auto plan = minimal_plan();
    plan.manifest.buildConfig.cxxflags = {"-DROOT_FLAG=1"};
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .object = "obj/main.o",
        .packageName = "root_flag_test",
        .packageCxxflags = {"-DROOT_FLAG=1"},
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_EQ(count_occurrences(ninja, "unit_cxxflags = -DROOT_FLAG=1"), 2u)
        << ninja;
    EXPECT_EQ(ninja.find("cxxflags  = -std=c++23 -O2 -DROOT_FLAG=1"), std::string::npos)
        << ninja;
}
