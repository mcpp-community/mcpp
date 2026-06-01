#include <gtest/gtest.h>

import std;
import mcpp.toolchain.clang;
import mcpp.toolchain.gcc;
import mcpp.toolchain.model;

using namespace mcpp::toolchain;

namespace {

Toolchain gcc_toolchain() {
    Toolchain tc;
    tc.compiler = CompilerId::GCC;
    tc.version = "16.1.0";
    tc.binaryPath = "g++";
    tc.targetTriple = "x86_64-linux-gnu";
    tc.stdlibId = "libstdc++";
    tc.stdlibVersion = "16.1.0";
    tc.stdModuleSource = "bits/std.cc";
    return tc;
}

Toolchain clang_toolchain() {
    Toolchain tc;
    tc.compiler = CompilerId::Clang;
    tc.version = "20.1.7";
    tc.binaryPath = "clang++";
    tc.targetTriple = "x86_64-linux-gnu";
    tc.stdlibId = "libc++";
    tc.stdlibVersion = "20.1.7";
    tc.stdModuleSource = "std.cppm";
    tc.stdCompatSource = "std.compat.cppm";
    return tc;
}

}  // namespace

TEST(ToolchainStdmod, GccStdModuleCommandUsesRequestedStandard) {
    auto cmd = gcc::std_module_build_command(
        gcc_toolchain(), "cache", "", "-std=c++26");

    EXPECT_NE(cmd.find("-std=c++26"), std::string::npos) << cmd;
    EXPECT_EQ(cmd.find("-std=c++23"), std::string::npos) << cmd;
}

TEST(ToolchainStdmod, ClangStdModuleCommandsUseRequestedStandard) {
    auto cmds = clang::std_module_build_commands(
        clang_toolchain(), "cache", "cache/pcm.cache/std.pcm", "", "-std=c++26");

    ASSERT_EQ(cmds.size(), 2u);
    for (auto const& cmd : cmds) {
        EXPECT_NE(cmd.find("-std=c++26"), std::string::npos) << cmd;
        EXPECT_EQ(cmd.find("-std=c++23"), std::string::npos) << cmd;
    }
}

TEST(ToolchainStdmod, ClangStdCompatCommandsUseRequestedStandard) {
    auto cmds = clang::std_compat_build_commands(
        clang_toolchain(),
        "cache",
        "cache/pcm.cache/std.compat.pcm",
        "cache/pcm.cache/std.pcm",
        "",
        "-std=c++26");

    ASSERT_EQ(cmds.size(), 2u);
    for (auto const& cmd : cmds) {
        EXPECT_NE(cmd.find("-std=c++26"), std::string::npos) << cmd;
        EXPECT_EQ(cmd.find("-std=c++23"), std::string::npos) << cmd;
    }
}
