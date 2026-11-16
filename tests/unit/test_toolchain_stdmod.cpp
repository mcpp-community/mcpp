
import std;
import mcpplibs.mtest;
import mcpp.toolchain.clang;
import mcpp.toolchain.gcc;
import mcpp.toolchain.model;

using namespace mcpplibs::mtest;

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

UTest gcc_std_module_command_uses_requested_standard{"ToolchainStdmod/GccStdModuleCommandUsesRequestedStandard", [] {
    auto cmd = gcc::std_module_build_command(
        gcc_toolchain(), "cache", "", "-std=c++26");

    expect(ne(cmd.find("-std=c++26"), std::string::npos)) << cmd;
    expect(eq(cmd.find("-std=c++23"), std::string::npos)) << cmd;
}};

UTest clang_std_module_commands_use_requested_standard{"ToolchainStdmod/ClangStdModuleCommandsUseRequestedStandard", [] {
    auto cmds = clang::std_module_build_commands(
        clang_toolchain(), "cache", "cache/pcm.cache/std.pcm", "", "-std=c++26");

    expect(fatal(eq(cmds.size(), 2u)));
    for (auto const& cmd : cmds) {
        expect(ne(cmd.find("-std=c++26"), std::string::npos)) << cmd;
        expect(eq(cmd.find("-std=c++23"), std::string::npos)) << cmd;
    }
}};

UTest clang_std_compat_commands_use_requested_standard{"ToolchainStdmod/ClangStdCompatCommandsUseRequestedStandard", [] {
    auto cmds = clang::std_compat_build_commands(
        clang_toolchain(),
        "cache",
        "cache/pcm.cache/std.compat.pcm",
        "cache/pcm.cache/std.pcm",
        "",
        "-std=c++26");

    expect(fatal(eq(cmds.size(), 2u)));
    for (auto const& cmd : cmds) {
        expect(ne(cmd.find("-std=c++26"), std::string::npos)) << cmd;
        expect(eq(cmd.find("-std=c++23"), std::string::npos)) << cmd;
    }
}};

UTest msvc_compat_source_is_parsed_as_a_module_but_its_bmi_is_not{"ToolchainStdmod/MsvcCompatSourceIsParsedAsAModuleButItsBmiIsNot", [] {
    auto tc = clang_toolchain();
    tc.stdModuleSource = "std.ixx";
    tc.stdCompatSource = "std.compat.ixx";
    auto cmds = clang::std_compat_build_commands(
        tc, "cache", "cache/pcm.cache/std.compat.pcm",
        "cache/pcm.cache/std.pcm", "", "-std=c++23", "-fms-runtime-lib=dll");
    expect(fatal(eq(cmds.size(), 2u)));
    expect(ne(cmds[0].find("-x c++-module"), std::string::npos));
    expect(eq(cmds[1].find("-x c++-module"), std::string::npos));
    for (auto const& cmd : cmds) {
        expect(ne(cmd.find("-fmodule-file=std="), std::string::npos));
        expect(ne(cmd.find("-fms-runtime-lib=dll"), std::string::npos));
    }
    tc.stdCompatSource = "std.compat.cppm";
    cmds = clang::std_compat_build_commands(
        tc, "cache", "cache/pcm.cache/std.compat.pcm",
        "cache/pcm.cache/std.pcm", "", "-std=c++23");
    expect(eq(cmds[0].find("-x c++-module"), std::string::npos));
}};

// THE PRECOMPILE HAS TO KNOW WHICH MACHINE, AND ONLY ONE OF TWO SOURCES EVER
// CARRIES IT.
//
// `stdModuleTargetFlags` reached only the CODEGEN command, on the reading that
// the first step needs headers and the second needs the machine. The first step
// needs both: a `--precompile` that does not say which target resolves the
// standard library's own `#include <__config>` against the BUILDING machine,
// and the error names a header rather than the missing flag.
//
// It was invisible while exactly two kinds of toolchain existed. A payload
// whose compiler IS its target needs no flag, and a PACKAGE-provided module
// carries the target inside `stdModuleFlags`. A payload whose compiler serves
// SEVERAL targets is a third kind and has neither -- one NDK clang++ compiles
// for both Android ABIs and is told which by `--target` alone.
UTest the_precompile_carries_the_machine_when_only_target_flags_have_it{"ToolchainStdmod/ThePrecompileCarriesTheMachineWhenOnlyTargetFlagsHaveIt", [] {
    auto tc = clang_toolchain();
    tc.targetTriple = "aarch64-linux-android";
    // What prepare_build sets for such a row: the machine, and nothing about
    // include paths, because the SDK's own driver finds those.
    tc.stdModuleTargetFlags =
        " --target=aarch64-unknown-linux-android21 -D__BIONIC_CTYPE_INLINE=";
    expect(fatal(tc.stdModuleFlags.empty()));

    auto cmds = clang::std_module_build_commands(
        tc, "cache", "cache/pcm.cache/std.pcm", "", "-std=c++23");
    expect(fatal(eq(cmds.size(), 2u)));

    // BOTH commands, not just the second. The precompile is the one that was
    // missing it and the one whose failure names a header.
    for (auto const& cmd : cmds) {
        expect(ne(cmd.find("--target=aarch64-unknown-linux-android21"), std::string::npos)) << cmd;
    }
    // And the bionic workaround reaches the step that parses the headers.
    expect(ne(cmds[0].find("-D__BIONIC_CTYPE_INLINE="), std::string::npos))
        << cmds[0];
}};

// AND IT IS NOT ADDED TWICE. `stdModuleFlags` is a SUPERSET of
// `stdModuleTargetFlags` when it is set at all -- its producer builds the
// machine part first and appends the include part -- so a precompile that
// concatenated both would put `--target=` on the command line twice. Taking
// the superset in preference is what keeps that from happening.
UTest a_package_provided_module_still_states_the_machine_exactly_once{"ToolchainStdmod/APackageProvidedModuleStillStatesTheMachineExactlyOnce", [] {
    auto tc = clang_toolchain();
    tc.targetTriple = "aarch64-macos";
    tc.stdModuleTargetFlags = " --target=arm64-apple-macos14.0";
    tc.stdModuleFlags =
        " --target=arm64-apple-macos14.0 -nostdinc++ -isystem /pkg/include";

    auto cmds = clang::std_module_build_commands(
        tc, "cache", "cache/pcm.cache/std.pcm", "", "-std=c++23");
    expect(fatal(eq(cmds.size(), 2u)));

    const auto count = [](std::string_view hay, std::string_view needle) {
        std::size_t n = 0, at = 0;
        while ((at = hay.find(needle, at)) != std::string_view::npos) {
            ++n; at += needle.size();
        }
        return n;
    };
    if (auto&& failed = expect(eq(count(cmds[0], "--target="), 1u)); !failed) failed << cmds[0];
    // The package's include path reaches the step that needs it, and only it.
    expect(ne(cmds[0].find("-isystem /pkg/include"), std::string::npos))
        << cmds[0];
    if (auto&& failed = expect(eq(cmds[1].find("-isystem /pkg/include"), std::string::npos)); !failed) failed << cmds[1];
}};
