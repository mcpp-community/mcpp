#include <gtest/gtest.h>

import std;
import mcpp.build.ninja;
import mcpp.build.plan;
import mcpp.manifest;
import mcpp.toolchain.model;

using namespace mcpp::build;

namespace {

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
