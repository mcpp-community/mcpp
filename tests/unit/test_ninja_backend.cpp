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

std::string escaped_include_flag(const std::filesystem::path& path) {
    auto s = path.string();
    std::string escaped;
    escaped.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '$' || c == ':')
            escaped.push_back('$');
        escaped.push_back(c);
    }
    return "-I" + escaped;
}

BuildPlan minimal_plan() {
    BuildPlan plan;
    plan.projectRoot = std::filesystem::temp_directory_path() / "mcpp-ninja-test";
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

TEST(NinjaBackend, CxxFlagsIncludeBuildIncludeDirs) {
    auto plan = minimal_plan();
    plan.manifest.buildConfig.includeDirs = {"include", "third_party/imgui"};

    auto flags = compute_flags(plan);

    EXPECT_NE(flags.cxx.find(escaped_include_flag(plan.projectRoot / "include")),
              std::string::npos)
        << flags.cxx;
    EXPECT_NE(flags.cxx.find(escaped_include_flag(
                  plan.projectRoot / std::filesystem::path{"third_party/imgui"})),
              std::string::npos)
        << flags.cxx;
}

// ── assembly sources (.S/.s → asm_object via $cc, .asm → nasm_object) ────────

TEST(NinjaBackend, GasSourceUsesAsmObjectRule) {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/copy.S",
        .object = "obj/copy.S.o",
        .packageName = "asm_rule_test",
        // Only the -D/-U/-I subset may reach the assembler: -std/-O/-w on an
        // asm command line are driver noise (or errors).
        .packageCflags = {"-DHAVE_ASM=1", "-std=c99", "-O2", "-w"},
        .packageCxxflags = {"-DWRONG_CXX_FLAG=1"},
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_NE(ninja.find("rule asm_object"), std::string::npos) << ninja;
    EXPECT_NE(ninja.find("build obj/copy.S.o : asm_object src/copy.S"),
              std::string::npos) << ninja;
    EXPECT_NE(ninja.find("cc        = "), std::string::npos) << ninja;
    EXPECT_NE(ninja.find("unit_asmflags = -DHAVE_ASM=1\n"), std::string::npos)
        << ninja;
    EXPECT_EQ(ninja.find("unit_asmflags = -DHAVE_ASM=1 -std=c99"), std::string::npos)
        << ninja;
    // The global asm flag string must not carry a C standard or opt level.
    auto asmline_pos = ninja.find("asmflags  =");
    ASSERT_NE(asmline_pos, std::string::npos) << ninja;
    auto asmline = ninja.substr(asmline_pos, ninja.find('\n', asmline_pos) - asmline_pos);
    EXPECT_EQ(asmline.find("-std="), std::string::npos) << asmline;
    EXPECT_EQ(asmline.find("-O"), std::string::npos) << asmline;
}

TEST(NinjaBackend, NasmSourceUsesNasmRuleWithDerivedFormat) {
    auto plan = minimal_plan();
    plan.nasmPath = "/opt/bin/nasm";
    plan.nasmFormat = "elf64";
    plan.compileUnits.push_back({
        .source = "src/simd.asm",
        .object = "obj/simd.asm.o",
        .packageName = "nasm_rule_test",
        .packageCflags = {"-DHAVE_AVX2=1", "-O2"},
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_NE(ninja.find("rule nasm_object"), std::string::npos) << ninja;
    EXPECT_NE(ninja.find("nasm      = /opt/bin/nasm"), std::string::npos) << ninja;
    EXPECT_NE(ninja.find("nasmfmt   = elf64"), std::string::npos) << ninja;
    EXPECT_NE(ninja.find("build obj/simd.asm.o : nasm_object src/simd.asm"),
              std::string::npos) << ninja;
    EXPECT_NE(ninja.find("unit_asmflags = -DHAVE_AVX2=1\n"), std::string::npos)
        << ninja;
}

TEST(NinjaBackend, NoAsmRulesWithoutAsmSources) {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .object = "obj/main.o",
        .packageName = "plain_test",
    });

    auto ninja = emit_ninja_string(plan);

    EXPECT_EQ(ninja.find("rule asm_object"), std::string::npos) << ninja;
    EXPECT_EQ(ninja.find("rule nasm_object"), std::string::npos) << ninja;
}

TEST(NinjaBackend, CompileCommandsSkipNasmAndCoverGas) {
    auto plan = minimal_plan();
    plan.nasmPath = "/opt/bin/nasm";
    plan.nasmFormat = "elf64";
    plan.compileUnits.push_back({
        .source = "src/simd.asm",
        .object = "obj/simd.asm.o",
        .packageName = "cdb_test",
    });
    plan.compileUnits.push_back({
        .source = "src/copy.S",
        .object = "obj/copy.S.o",
        .packageName = "cdb_test",
    });

    auto flags = compute_flags(plan);
    auto cdb = emit_compile_commands(plan, flags);

    // NASM command lines are meaningless to CDB consumers (clangd) — excluded.
    EXPECT_EQ(cdb.find("simd.asm"), std::string::npos) << cdb;
    // GAS units ride the C driver and stay in the CDB.
    EXPECT_NE(cdb.find("copy.S"), std::string::npos) << cdb;
    EXPECT_EQ(cdb.find("\"-std=c11\""), std::string::npos) << cdb;   // asm-safe flags, no C std
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
