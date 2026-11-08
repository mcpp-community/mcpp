
import std;
import mcpplibs.mtest;
import mcpp.build.build_program;

using namespace mcpplibs::mtest;

// #613: the part of a build program's environment an install hook receives.
// The names, their order, and the rule for each value are stated here once for
// every platform; tests/e2e/648 shows a real install hook reading them.

namespace {

using HookEnv = std::vector<std::pair<std::string, std::string>>;

std::string value_of(const HookEnv& env, std::string_view key) {
    for (auto const& [k, v] : env)
        if (k == key) return v;
    expect(false) << key << " is missing";
    return {};
}

}  // namespace

UTest names_the_six_variables_in_order{"InstallHookEnv/NamesTheSixVariablesInOrder", [] {
    auto env = mcpp::build::install_hook_env(mcpp::build::BuildProgramEnv{});
    std::vector<std::string> names;
    for (auto const& [k, v] : env) names.push_back(k);
    expect(eq(names, (std::vector<std::string>{
        "MCPP_COMPILER", "MCPP_CXX_STDLIB", "MCPP_TARGET",
        "MCPP_TARGET_OS", "MCPP_TARGET_ARCH", "MCPP_TARGET_ENV"})));
}};

UTest without_a_toolchain_the_toolchain_values_are_present_and_empty{"InstallHookEnv/WithoutAToolchainTheToolchainValuesArePresentAndEmpty", [] {
    auto env = mcpp::build::install_hook_env(mcpp::build::BuildProgramEnv{});
    expect(eq(value_of(env, "MCPP_COMPILER"), ""));
    expect(eq(value_of(env, "MCPP_CXX_STDLIB"), ""));
}};

UTest a_native_build_names_the_host{"InstallHookEnv/ANativeBuildNamesTheHost", [] {
    mcpp::build::BuildProgramEnv bp;
    bp.compilerId = "gcc";
    bp.cxxStdlib = "libstdc++";
    auto env = mcpp::build::install_hook_env(bp);
    expect(eq(value_of(env, "MCPP_COMPILER"), "gcc"));
    expect(eq(value_of(env, "MCPP_CXX_STDLIB"), "libstdc++"));
    const auto target = value_of(env, "MCPP_TARGET");
    expect(fatal(!target.empty()));
    expect(!(value_of(env, "MCPP_TARGET_OS").empty())) << target;
    expect(target.starts_with(value_of(env, "MCPP_TARGET_ARCH"))) << target;
}};

UTest a_cross_build_splits_the_requested_triple{"InstallHookEnv/ACrossBuildSplitsTheRequestedTriple", [] {
    mcpp::build::BuildProgramEnv bp;
    bp.targetTriple = "aarch64-linux-android";
    bp.compilerId = "clang";
    bp.cxxStdlib = "libc++";
    auto env = mcpp::build::install_hook_env(bp);
    expect(eq(value_of(env, "MCPP_TARGET"), "aarch64-linux-android"));
    expect(eq(value_of(env, "MCPP_TARGET_OS"), "linux"));
    expect(eq(value_of(env, "MCPP_TARGET_ARCH"), "aarch64"));
    expect(eq(value_of(env, "MCPP_TARGET_ENV"), "android"));
    expect(eq(value_of(env, "MCPP_CXX_STDLIB"), "libc++"));
}};
