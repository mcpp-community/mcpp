#include <gtest/gtest.h>

import std;
import mcpp.build.build_program;

// #613: the part of a build program's environment an install hook receives.
// The names, their order, and the rule for each value are stated here once for
// every platform; tests/e2e/648 shows a real install hook reading them.

namespace {

using HookEnv = std::vector<std::pair<std::string, std::string>>;

std::string value_of(const HookEnv& env, std::string_view key) {
    for (auto const& [k, v] : env)
        if (k == key) return v;
    ADD_FAILURE() << key << " is missing";
    return {};
}

}  // namespace

TEST(InstallHookEnv, NamesTheSixVariablesInOrder) {
    auto env = mcpp::build::install_hook_env(mcpp::build::BuildProgramEnv{});
    std::vector<std::string> names;
    for (auto const& [k, v] : env) names.push_back(k);
    EXPECT_EQ(names, (std::vector<std::string>{
        "MCPP_COMPILER", "MCPP_CXX_STDLIB", "MCPP_TARGET",
        "MCPP_TARGET_OS", "MCPP_TARGET_ARCH", "MCPP_TARGET_ENV"}));
}

TEST(InstallHookEnv, WithoutAToolchainTheToolchainValuesArePresentAndEmpty) {
    auto env = mcpp::build::install_hook_env(mcpp::build::BuildProgramEnv{});
    EXPECT_EQ(value_of(env, "MCPP_COMPILER"), "");
    EXPECT_EQ(value_of(env, "MCPP_CXX_STDLIB"), "");
}

TEST(InstallHookEnv, ANativeBuildNamesTheHost) {
    mcpp::build::BuildProgramEnv bp;
    bp.compilerId = "gcc";
    bp.cxxStdlib = "libstdc++";
    auto env = mcpp::build::install_hook_env(bp);
    EXPECT_EQ(value_of(env, "MCPP_COMPILER"), "gcc");
    EXPECT_EQ(value_of(env, "MCPP_CXX_STDLIB"), "libstdc++");
    const auto target = value_of(env, "MCPP_TARGET");
    ASSERT_FALSE(target.empty());
    EXPECT_FALSE(value_of(env, "MCPP_TARGET_OS").empty()) << target;
    EXPECT_TRUE(target.starts_with(value_of(env, "MCPP_TARGET_ARCH"))) << target;
}

TEST(InstallHookEnv, ACrossBuildSplitsTheRequestedTriple) {
    mcpp::build::BuildProgramEnv bp;
    bp.targetTriple = "aarch64-linux-android";
    bp.compilerId = "clang";
    bp.cxxStdlib = "libc++";
    auto env = mcpp::build::install_hook_env(bp);
    EXPECT_EQ(value_of(env, "MCPP_TARGET"), "aarch64-linux-android");
    EXPECT_EQ(value_of(env, "MCPP_TARGET_OS"), "linux");
    EXPECT_EQ(value_of(env, "MCPP_TARGET_ARCH"), "aarch64");
    EXPECT_EQ(value_of(env, "MCPP_TARGET_ENV"), "android");
    EXPECT_EQ(value_of(env, "MCPP_CXX_STDLIB"), "libc++");
}
