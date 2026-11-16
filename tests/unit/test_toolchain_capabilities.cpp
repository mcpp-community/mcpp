// A GCC's C++ modules by its version: P1689 and -fmodules arrived together in GCC 14, and an
// earlier GCC (DJGPP's 12.2.0, measured) refuses both, so it is given no module flag, its units
// that need modules are refused at planning, and the rest build (design 2026-11-12 §15.3).
import std;
import mcpplibs.mtest;
import mcpp.toolchain.model;
import mcpp.toolchain.provider;

using namespace mcpplibs::mtest;
using namespace mcpp::toolchain;

namespace {

ProviderCapabilities gcc(std::string version) {
    Toolchain tc;
    tc.compiler = CompilerId::GCC;
    tc.version = std::move(version);
    return capabilities_for(tc);
}

UTest gcc_before_14_has_no_modules{"ToolchainCapabilities/GccBefore14HasNoModules", [] {
    for (auto v : {"12.2.0", "13.3.0"}) {
        const auto caps = gcc(v);
        expect(!caps.has_modules) << v;
        expect(!caps.has_builtin_p1689_scan) << v;
    }
}};

UTest gcc_14_and_later_has_modules{"ToolchainCapabilities/Gcc14AndLaterHasModules", [] {
    for (auto v : {"14.1.0", "16.1.0", "17.0.0-20261108"}) {
        const auto caps = gcc(v);
        expect(caps.has_modules) << v;
        expect(caps.has_builtin_p1689_scan) << v;
    }
}};

UTest gcc_without_modules_is_given_no_module_flag{"ToolchainCapabilities/GccWithoutModulesIsGivenNoModuleFlag", [] {
    Toolchain tc;
    tc.compiler = CompilerId::GCC;
    tc.version = "12.2.0";
    expect(eq(std::string(bmi_traits(tc).compileModulesFlag), std::string{}));
    tc.version = "16.1.0";
    expect(eq(std::string(bmi_traits(tc).compileModulesFlag), std::string{" -fmodules"}));
}};

UTest an_unparsed_gcc_version_is_current{"ToolchainCapabilities/AnUnparsedGccVersionIsCurrent", [] {
    expect(gcc("").has_modules);
    expect(gcc("trunk").has_modules);
}};

}  // namespace
