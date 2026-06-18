#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.platform;

TEST(Manifest, MinimalValid) {
    constexpr auto src = R"(
[package]
name = "hello"
version = "0.1.0"
[language]
standard = "c++23"
[modules]
sources = ["src/**/*.cppm"]
[targets.hello]
kind = "bin"
main = "src/main.cpp"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_EQ(m->package.name,    "hello");
    EXPECT_EQ(m->package.version, "0.1.0");
    EXPECT_EQ(m->language.standard, "c++23");
    EXPECT_TRUE(m->language.modules);
    ASSERT_EQ(m->targets.size(), 1u);
    EXPECT_EQ(m->targets[0].name, "hello");
    EXPECT_EQ(m->targets[0].kind, mcpp::manifest::Target::Binary);
}

TEST(Manifest, SharedTargetSoname) {
    constexpr auto src = R"(
[package]
name = "dep"
version = "0.1.0"
[build]
sources = ["src/*.c"]
[targets.dep]
kind = "shared"
soname = "libdep.so.1"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->targets.size(), 1u);
    EXPECT_EQ(m->targets[0].kind, mcpp::manifest::Target::SharedLibrary);
    EXPECT_EQ(m->targets[0].soname, "libdep.so.1");
}

TEST(Manifest, RejectsSonameOnNonSharedTarget) {
    constexpr auto src = R"(
[package]
name = "app"
version = "0.1.0"
[targets.app]
kind = "bin"
main = "src/main.cpp"
soname = "libapp.so.1"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("soname is only valid for shared targets"),
              std::string::npos);
}

TEST(Manifest, PackageStandardCpp26AcceptedAndMirrored) {
    constexpr auto src = R"(
[package]
name = "hello26"
version = "0.1.0"
standard = "c++26"
[modules]
sources = ["src/**/*.cppm"]
[targets.hello26]
kind = "bin"
main = "src/main.cpp"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_EQ(m->package.standard, "c++26");
    EXPECT_EQ(m->language.standard, "c++26");
}

TEST(Manifest, LegacyLanguageCpp2cNormalizesToCpp26) {
    constexpr auto src = R"(
[package]
name = "hello26"
version = "0.1.0"
[language]
standard = "c++2c"
[modules]
sources = ["src/**/*.cppm"]
[targets.hello26]
kind = "bin"
main = "src/main.cpp"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_EQ(m->package.standard, "c++26");
    EXPECT_EQ(m->language.standard, "c++26");
}

TEST(Manifest, RejectsStdFlagInCxxflags) {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[build]
cxxflags = ["-std=c++26"]
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
)");
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("[package].standard"), std::string::npos)
        << m.error().message;
}

TEST(Manifest, RejectMissingVersion) {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
)");
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("package.version"), std::string::npos);
}

TEST(Manifest, RejectImportStdWithoutCpp23) {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[language]
standard = "c++20"
import_std = true
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
)");
    ASSERT_FALSE(m.has_value());
    // M5.0: validator now bails on c++20 first ("MVP only supports c++23"),
    // before reaching an import_std-specific check. Either signal is fine.
    auto& msg = m.error().message;
    EXPECT_TRUE(msg.find("import_std") != std::string::npos
             || msg.find("c++23")      != std::string::npos)
        << "actual: " << msg;
}

TEST(Manifest, RejectModulesFalse) {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[language]
standard = "c++23"
modules = false
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
)");
    ASSERT_FALSE(m.has_value());
}

TEST(Manifest, ParsesDependencies) {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
[dependencies]
"mcpplibs.primitives" = "0.0.1"
[dev-dependencies]
"gtest" = "1.15.2"
)");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_EQ(m->dependencies.size(), 1u);
    EXPECT_EQ(m->dependencies.at("mcpplibs.primitives").version, "0.0.1");
    EXPECT_EQ(m->devDependencies.at("gtest").version, "1.15.2");
}

TEST(Manifest, ParsesDependencyVisibility) {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
[dependencies.compat]
imgui = { version = "1.92.8", visibility = "private" }
glfw = { version = "3.4", visibility = "interface" }
opengl = "2026.05.31"
)");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->dependencies.size(), 3u);
    EXPECT_EQ(m->dependencies.at("compat.imgui").visibility, "private");
    EXPECT_EQ(m->dependencies.at("compat.glfw").visibility, "interface");
    EXPECT_EQ(m->dependencies.at("compat.opengl").visibility, "public");
}

TEST(Manifest, RejectsInvalidDependencyVisibility) {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[dependencies.compat]
imgui = { version = "1.92.8", visibility = "implementation" }
)");
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("visibility"), std::string::npos);
}

TEST(Manifest, DefaultTemplateRoundTrip) {
    auto src = mcpp::manifest::default_template("hello");
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_EQ(m->package.name, "hello");
}

TEST(ListXpkgVersions, MultipleEntriesAcrossPlatforms) {
    constexpr auto src = R"(
package = {
    name = "foo",
    xpm = {
        linux = {
            ["0.1.0"] = { url = "u1", sha256 = "x" },
            ["0.2.0"] = { url = "u2", sha256 = "y" },
            ["1.0.0"] = { url = "u3", sha256 = "z" },
        },
        macosx = {
            ["0.1.0"] = { url = "u1m", sha256 = "x" },
        },
    },
}
)";
    auto linux = mcpp::manifest::list_xpkg_versions(src, "linux");
    ASSERT_EQ(linux.size(), 3u);
    EXPECT_EQ(linux[0], "0.1.0");
    EXPECT_EQ(linux[1], "0.2.0");
    EXPECT_EQ(linux[2], "1.0.0");

    auto mac = mcpp::manifest::list_xpkg_versions(src, "macosx");
    ASSERT_EQ(mac.size(), 1u);
    EXPECT_EQ(mac[0], "0.1.0");

    auto win = mcpp::manifest::list_xpkg_versions(src, "windows");
    EXPECT_TRUE(win.empty());
}

TEST(ListXpkgVersions, MissingXpmReturnsEmpty) {
    constexpr auto src = R"(package = { name = "foo" })";
    EXPECT_TRUE(mcpp::manifest::list_xpkg_versions(src, "linux").empty());
}

TEST(Manifest, BuildCflagsCxxflagsAndCStandard) {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
sources    = ["src/**/*.{cppm,c}"]
cflags     = ["-Wall", "-DFOO=1"]
cxxflags   = ["-Wextra"]
ldflags    = ["-lfoo", "-Wl,--as-needed"]
c_standard = "c11"
[targets.x]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->buildConfig.cflags.size(), 2u);
    EXPECT_EQ(m->buildConfig.cflags[0], "-Wall");
    EXPECT_EQ(m->buildConfig.cflags[1], "-DFOO=1");
    ASSERT_EQ(m->buildConfig.cxxflags.size(), 1u);
    EXPECT_EQ(m->buildConfig.cxxflags[0], "-Wextra");
    ASSERT_EQ(m->buildConfig.ldflags.size(), 2u);
    EXPECT_EQ(m->buildConfig.ldflags[0], "-lfoo");
    EXPECT_EQ(m->buildConfig.ldflags[1], "-Wl,--as-needed");
    EXPECT_EQ(m->buildConfig.cStandard, "c11");
}

TEST(Manifest, BuildMacosDeploymentTarget) {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
macos_deployment_target = "11.0"
[targets.x]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_EQ(m->buildConfig.macosDeploymentTarget, "11.0");
}

TEST(Manifest, BuildMacosDeploymentTargetDefaultsEmpty) {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[targets.x]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_TRUE(m->buildConfig.macosDeploymentTarget.empty());
}

TEST(Manifest, RuntimeConfig) {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[runtime]
library_dirs = ["runtime/lib", "plugins"]
dlopen_libs = ["libGLX.so.0", "libGL.so.1"]
capabilities = ["x11.display", "opengl.glx.driver"]
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->runtimeConfig.libraryDirs.size(), 2u);
    EXPECT_EQ(m->runtimeConfig.libraryDirs[0], "runtime/lib");
    EXPECT_EQ(m->runtimeConfig.libraryDirs[1], "plugins");
    ASSERT_EQ(m->runtimeConfig.dlopenLibs.size(), 2u);
    EXPECT_EQ(m->runtimeConfig.dlopenLibs[0], "libGLX.so.0");
    EXPECT_EQ(m->runtimeConfig.dlopenLibs[1], "libGL.so.1");
    ASSERT_EQ(m->runtimeConfig.capabilities.size(), 2u);
    EXPECT_EQ(m->runtimeConfig.capabilities[0], "x11.display");
    EXPECT_EQ(m->runtimeConfig.capabilities[1], "opengl.glx.driver");
}

TEST(SynthesizeFromXpkgLua, CflagsCxxflagsLdflagsAndCStandard) {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "tinyc",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources    = { "*/src/*.c" },
        cflags     = { "-Wall", "-Dunused" },
        cxxflags   = { "-Wextra" },
        ldflags    = { "-ltinyc" },
        c_standard = "c11",
        targets    = { ["tinyc"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "tinyc", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->buildConfig.cflags.size(), 2u);
    EXPECT_EQ(m->buildConfig.cflags[0], "-Wall");
    EXPECT_EQ(m->buildConfig.cflags[1], "-Dunused");
    ASSERT_EQ(m->buildConfig.cxxflags.size(), 1u);
    EXPECT_EQ(m->buildConfig.cxxflags[0], "-Wextra");
    ASSERT_EQ(m->buildConfig.ldflags.size(), 1u);
    EXPECT_EQ(m->buildConfig.ldflags[0], "-ltinyc");
    EXPECT_EQ(m->buildConfig.cStandard, "c11");
    ASSERT_EQ(m->modules.sources.size(), 1u);
    EXPECT_EQ(m->modules.sources[0], "*/src/*.c");
}

TEST(SynthesizeFromXpkgLua, SharedTargetSoname) {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "tinyshared",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/*.c" },
        targets = { ["tinyshared"] = { kind = "shared", soname = "libtinyshared.so.1" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "tinyshared", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->targets.size(), 1u);
    EXPECT_EQ(m->targets[0].kind, mcpp::manifest::Target::SharedLibrary);
    EXPECT_EQ(m->targets[0].soname, "libtinyshared.so.1");
}

TEST(SynthesizeFromXpkgLua, RuntimeConfig) {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "glfw",
    xpm  = { linux = { ["3.4"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/context.c" },
        runtime = {
            library_dirs = { "mcpp_generated/runtime/lib" },
            dlopen_libs = { "libGLX.so.0", "libGL.so.1" },
            capabilities = { "x11.display", "opengl.glx.driver" },
        },
        targets = { ["glfw"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "glfw", "3.4");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->runtimeConfig.libraryDirs.size(), 1u);
    EXPECT_EQ(m->runtimeConfig.libraryDirs[0], "mcpp_generated/runtime/lib");
    ASSERT_EQ(m->runtimeConfig.dlopenLibs.size(), 2u);
    EXPECT_EQ(m->runtimeConfig.dlopenLibs[0], "libGLX.so.0");
    EXPECT_EQ(m->runtimeConfig.dlopenLibs[1], "libGL.so.1");
    ASSERT_EQ(m->runtimeConfig.capabilities.size(), 2u);
    EXPECT_EQ(m->runtimeConfig.capabilities[0], "x11.display");
    EXPECT_EQ(m->runtimeConfig.capabilities[1], "opengl.glx.driver");
}

TEST(SynthesizeFromXpkgLua, AppliesCurrentPlatformMcppOverlay) {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "tinyc",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources      = { "*/src/common.c" },
        include_dirs = { "*/include" },
        cflags       = { "-DCOMMON=1" },
        deps         = { ["compat.base"] = "1.0.0" },
        targets      = { ["tinyc"] = { kind = "lib" } },
        linux = {
            sources      = { "*/src/linux.c" },
            include_dirs = { "*/src/linux" },
            cflags       = { "-DLINUX=1" },
            deps         = { ["compat.x11"] = "1.8.13" },
        },
        macosx = {
            sources      = { "*/src/cocoa.m" },
            include_dirs = { "*/src/macos" },
            cflags       = { "-DMACOS=1" },
            deps         = { ["compat.cocoa"] = "1.0.0" },
        },
        windows = {
            sources      = { "*/src/win32.c" },
            include_dirs = { "*/src/win32" },
            cflags       = { "-DWINDOWS=1" },
            deps         = { ["compat.win32"] = "1.0.0" },
        },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "tinyc", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();

    std::string expectedSource = "*/src/linux.c";
    std::string expectedInclude = "*/src/linux";
    std::string expectedCflag = "-DLINUX=1";
    std::string expectedDep = "compat.x11";
    if constexpr (mcpp::platform::is_macos) {
        expectedSource = "*/src/cocoa.m";
        expectedInclude = "*/src/macos";
        expectedCflag = "-DMACOS=1";
        expectedDep = "compat.cocoa";
    } else if constexpr (mcpp::platform::is_windows) {
        expectedSource = "*/src/win32.c";
        expectedInclude = "*/src/win32";
        expectedCflag = "-DWINDOWS=1";
        expectedDep = "compat.win32";
    }

    ASSERT_EQ(m->modules.sources.size(), 2u);
    EXPECT_EQ(m->modules.sources[0], "*/src/common.c");
    EXPECT_EQ(m->modules.sources[1], expectedSource);
    ASSERT_EQ(m->buildConfig.includeDirs.size(), 2u);
    EXPECT_EQ(m->buildConfig.includeDirs[0], "*/include");
    EXPECT_EQ(m->buildConfig.includeDirs[1], expectedInclude);
    ASSERT_EQ(m->buildConfig.cflags.size(), 2u);
    EXPECT_EQ(m->buildConfig.cflags[0], "-DCOMMON=1");
    EXPECT_EQ(m->buildConfig.cflags[1], expectedCflag);
    EXPECT_EQ(m->dependencies.count("compat.base"), 1u);
    EXPECT_EQ(m->dependencies.count(expectedDep), 1u);
}

TEST(SynthesizeFromXpkgLua, GeneratedFiles) {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "tinyc",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/*.c" },
        generated_files = {
            ["mcpp_generated/include/config.h"] = "#pragma once\n#define TINYC_OK 1\n",
        },
        include_dirs = { "mcpp_generated/include" },
        targets = { ["tinyc"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "tinyc", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->buildConfig.generatedFiles.size(), 1u);
    auto it = m->buildConfig.generatedFiles.find("mcpp_generated/include/config.h");
    ASSERT_NE(it, m->buildConfig.generatedFiles.end());
    EXPECT_EQ(it->second, "#pragma once\n#define TINYC_OK 1\n");
}

TEST(Manifest, DependenciesFlatDefaultNamespace) {
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"
[dependencies]
gtest = "1.15.2"
foo   = { path = "../foo" }
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->dependencies.size(), 2u);
    auto& g = m->dependencies.at("gtest");
    EXPECT_EQ(g.namespace_, "mcpplibs");
    EXPECT_EQ(g.shortName,  "gtest");
    EXPECT_EQ(g.version,    "1.15.2");
    auto& f = m->dependencies.at("foo");
    EXPECT_EQ(f.namespace_, "mcpplibs");
    EXPECT_EQ(f.shortName,  "foo");
    EXPECT_EQ(f.path,       "../foo");
}

TEST(Manifest, DependenciesNamespacedSubtable) {
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies.mcpplibs]
cmdline   = "0.0.2"
templates = { version = "0.0.1" }

[dependencies]
gtest = "1.15.2"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->dependencies.size(), 3u);

    auto& cmdline = m->dependencies.at("mcpplibs.cmdline");
    EXPECT_EQ(cmdline.namespace_, "mcpplibs");
    EXPECT_EQ(cmdline.shortName,  "cmdline");
    EXPECT_EQ(cmdline.version,    "0.0.2");
    EXPECT_FALSE(cmdline.legacyDottedKey);

    auto& tmpl = m->dependencies.at("mcpplibs.templates");
    EXPECT_EQ(tmpl.namespace_, "mcpplibs");
    EXPECT_EQ(tmpl.shortName,  "templates");
    EXPECT_EQ(tmpl.version,    "0.0.1");

    auto& gtest = m->dependencies.at("gtest");
    EXPECT_EQ(gtest.namespace_, "mcpplibs");
    EXPECT_EQ(gtest.shortName,  "gtest");
    EXPECT_EQ(gtest.version,    "1.15.2");
}

TEST(Manifest, DependenciesLegacyDottedKeyStillParsed) {
    // Pre-namespace-aware mcpp.toml: quoted dotted key.
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies]
"mcpplibs.cmdline" = "0.0.2"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->dependencies.size(), 1u);
    auto& s = m->dependencies.at("mcpplibs.cmdline");
    EXPECT_EQ(s.namespace_, "mcpplibs");
    EXPECT_EQ(s.shortName,  "cmdline");
    EXPECT_EQ(s.version,    "0.0.2");
    EXPECT_TRUE(s.legacyDottedKey);
}

TEST(Manifest, DependenciesDottedSelectorPreservesUserKeyAndCandidates) {
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies]
capi.lua = "0.0.3"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->dependencies.size(), 1u);

    auto& s = m->dependencies.at("capi.lua");
    EXPECT_EQ(s.namespace_, "mcpplibs.capi");
    EXPECT_EQ(s.shortName,  "lua");
    EXPECT_EQ(s.version,    "0.0.3");
    EXPECT_FALSE(s.legacyDottedKey);
    ASSERT_EQ(s.candidates.size(), 2u);
    EXPECT_EQ(s.candidates[0].namespace_, "mcpplibs.capi");
    EXPECT_EQ(s.candidates[0].shortName, "lua");
    EXPECT_EQ(s.candidates[1].namespace_, "capi");
    EXPECT_EQ(s.candidates[1].shortName, "lua");
}

TEST(Manifest, DependenciesNamespacedSubtableNestedDottedKeyIsCanonical) {
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies.mcpplibs]
capi.lua = "0.0.3"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->dependencies.size(), 1u);

    auto& s = m->dependencies.at("mcpplibs.capi.lua");
    EXPECT_EQ(s.namespace_, "mcpplibs.capi");
    EXPECT_EQ(s.shortName,  "lua");
    EXPECT_EQ(s.version,    "0.0.3");
    EXPECT_FALSE(s.legacyDottedKey);
    ASSERT_EQ(s.candidates.size(), 1u);
    EXPECT_EQ(s.candidates[0].namespace_, "mcpplibs.capi");
    EXPECT_EQ(s.candidates[0].shortName, "lua");
}

TEST(Manifest, DependenciesInlineSpecCoexistsWithSubtable) {
    // `bar = { git = "...", tag = "..." }` looks like a subtable but has
    // only dep-spec keys → treated as inline spec under default ns.
    // `[dependencies.acme]` is a real namespace subtable.
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies]
bar = { git = "https://example.com/bar.git", tag = "v1" }

[dependencies.acme]
util = "2.0.0"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->dependencies.size(), 2u);
    auto& bar = m->dependencies.at("bar");
    EXPECT_EQ(bar.namespace_, "mcpplibs");
    EXPECT_EQ(bar.shortName,  "bar");
    EXPECT_EQ(bar.git,        "https://example.com/bar.git");
    EXPECT_EQ(bar.gitRev,     "v1");
    EXPECT_EQ(bar.gitRefKind, "tag");

    auto& util = m->dependencies.at("acme.util");
    EXPECT_EQ(util.namespace_, "acme");
    EXPECT_EQ(util.shortName,  "util");
    EXPECT_EQ(util.version,    "2.0.0");
}

TEST(SynthesizeFromXpkgLua, DepsKeySplitNamespace) {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "consumer",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/*.cppm" },
        deps    = {
            ["mbedtls"]          = "3.6.1",
            ["mcpplibs.cmdline"] = "0.0.2",
        },
        targets = { ["consumer"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "consumer", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->dependencies.size(), 2u);

    auto& a = m->dependencies.at("mbedtls");
    EXPECT_EQ(a.namespace_, "mcpplibs");
    EXPECT_EQ(a.shortName,  "mbedtls");
    EXPECT_EQ(a.version,    "3.6.1");

    auto& b = m->dependencies.at("mcpplibs.cmdline");
    EXPECT_EQ(b.namespace_, "mcpplibs");
    EXPECT_EQ(b.shortName,  "cmdline");
    EXPECT_EQ(b.version,    "0.0.2");
}

TEST(SynthesizeFromXpkgLua, DepsDottedSelectorsUseManifestRules) {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "consumer",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/*.cppm" },
        deps    = {
            ["capi.lua"]     = "0.0.3",
            ["imgui.core"]   = "0.0.1",
            ["compat.gtest"] = "1.15.2",
        },
        targets = { ["consumer"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "consumer", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->dependencies.size(), 3u);

    auto& lua = m->dependencies.at("capi.lua");
    EXPECT_EQ(lua.namespace_, "mcpplibs.capi");
    EXPECT_EQ(lua.shortName, "lua");
    EXPECT_EQ(lua.version, "0.0.3");
    ASSERT_EQ(lua.candidates.size(), 2u);
    EXPECT_EQ(lua.candidates[1].namespace_, "capi");
    EXPECT_EQ(lua.candidates[1].shortName, "lua");

    auto& imgui = m->dependencies.at("imgui.core");
    EXPECT_EQ(imgui.namespace_, "mcpplibs.imgui");
    EXPECT_EQ(imgui.shortName, "core");
    EXPECT_EQ(imgui.version, "0.0.1");
    ASSERT_EQ(imgui.candidates.size(), 2u);
    EXPECT_EQ(imgui.candidates[1].namespace_, "imgui");
    EXPECT_EQ(imgui.candidates[1].shortName, "core");

    auto& gtest = m->dependencies.at("compat.gtest");
    EXPECT_EQ(gtest.namespace_, "mcpplibs.compat");
    EXPECT_EQ(gtest.shortName, "gtest");
    EXPECT_EQ(gtest.version, "1.15.2");
    ASSERT_EQ(gtest.candidates.size(), 2u);
    EXPECT_EQ(gtest.candidates[1].namespace_, "compat");
    EXPECT_EQ(gtest.candidates[1].shortName, "gtest");
}

TEST(Manifest, WorkspaceSectionParsed) {
    constexpr auto src = R"(
[workspace]
members = ["libs/core", "libs/http", "apps/server"]
exclude = ["libs/experimental"]

[workspace.dependencies]
cmdline = "0.0.2"

[workspace.dependencies.compat]
gtest = "1.15.2"
mbedtls = "3.6.1"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_TRUE(m->workspace.present);
    ASSERT_EQ(m->workspace.members.size(), 3u);
    EXPECT_EQ(m->workspace.members[0], "libs/core");
    EXPECT_EQ(m->workspace.members[1], "libs/http");
    EXPECT_EQ(m->workspace.members[2], "apps/server");
    ASSERT_EQ(m->workspace.exclude.size(), 1u);
    EXPECT_EQ(m->workspace.exclude[0], "libs/experimental");
    ASSERT_EQ(m->workspace.dependencies.size(), 3u);
    auto& gt = m->workspace.dependencies.at("compat.gtest");
    EXPECT_EQ(gt.version, "1.15.2");
    EXPECT_EQ(gt.namespace_, "compat");
}

TEST(Manifest, WorkspaceDependenciesUseDottedSelectorRules) {
    constexpr auto src = R"(
[workspace]
members = ["libs/core"]

[workspace.dependencies]
capi.lua = "0.0.3"
imgui.core = "0.0.1"
compat.gtest = "1.15.2"
mcpplibs.templates = "0.0.1"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->workspace.dependencies.size(), 4u);

    auto& lua = m->workspace.dependencies.at("capi.lua");
    EXPECT_EQ(lua.namespace_, "mcpplibs.capi");
    EXPECT_EQ(lua.shortName, "lua");
    EXPECT_EQ(lua.version, "0.0.3");
    ASSERT_EQ(lua.candidates.size(), 2u);
    EXPECT_EQ(lua.candidates[1].namespace_, "capi");
    EXPECT_EQ(lua.candidates[1].shortName, "lua");

    auto& imgui = m->workspace.dependencies.at("imgui.core");
    EXPECT_EQ(imgui.namespace_, "mcpplibs.imgui");
    EXPECT_EQ(imgui.shortName, "core");
    EXPECT_EQ(imgui.version, "0.0.1");
    ASSERT_EQ(imgui.candidates.size(), 2u);
    EXPECT_EQ(imgui.candidates[1].namespace_, "imgui");
    EXPECT_EQ(imgui.candidates[1].shortName, "core");

    auto& gt = m->workspace.dependencies.at("compat.gtest");
    EXPECT_EQ(gt.namespace_, "mcpplibs.compat");
    EXPECT_EQ(gt.shortName, "gtest");
    EXPECT_EQ(gt.version, "1.15.2");
    ASSERT_EQ(gt.candidates.size(), 2u);
    EXPECT_EQ(gt.candidates[1].namespace_, "compat");
    EXPECT_EQ(gt.candidates[1].shortName, "gtest");

    auto& tmpl = m->workspace.dependencies.at("mcpplibs.templates");
    EXPECT_EQ(tmpl.namespace_, "mcpplibs");
    EXPECT_EQ(tmpl.shortName, "templates");
    EXPECT_EQ(tmpl.version, "0.0.1");
    ASSERT_EQ(tmpl.candidates.size(), 1u);
    EXPECT_EQ(tmpl.candidates[0].namespace_, "mcpplibs");
    EXPECT_EQ(tmpl.candidates[0].shortName, "templates");
}

TEST(Manifest, WorkspaceTrueInDependency) {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[dependencies.compat]
mbedtls = { workspace = true }
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    auto& s = m->dependencies.at("compat.mbedtls");
    EXPECT_TRUE(s.inheritWorkspace);
    EXPECT_EQ(s.namespace_, "compat");
    EXPECT_EQ(s.shortName, "mbedtls");
}

TEST(Manifest, NoWorkspaceSectionMeansNotPresent) {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value());
    EXPECT_FALSE(m->workspace.present);
}

TEST(Manifest, LibRootInferredFromPackageName) {
    constexpr auto src = R"(
[package]
name    = "mcpplibs.tinyhttps"
version = "0.2.0"
[targets.tinyhttps]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_TRUE(m->lib.path.empty());
    EXPECT_TRUE(mcpp::manifest::has_lib_target(*m));
    auto root = mcpp::manifest::resolve_lib_root_path(*m);
    EXPECT_EQ(root, std::filesystem::path("src/tinyhttps.cppm"));
}

TEST(Manifest, LibRootBareNameNoNamespace) {
    constexpr auto src = R"(
[package]
name    = "gtest"
version = "1.0.0"
[targets.gtest]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    auto root = mcpp::manifest::resolve_lib_root_path(*m);
    EXPECT_EQ(root, std::filesystem::path("src/gtest.cppm"));
}

TEST(Manifest, LibRootExplicitOverride) {
    constexpr auto src = R"(
[package]
name    = "mcpplibs.tinyhttps"
version = "0.2.0"
[lib]
path = "src/api.cppm"
[targets.tinyhttps]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_EQ(m->lib.path.string(), "src/api.cppm");
    auto root = mcpp::manifest::resolve_lib_root_path(*m);
    EXPECT_EQ(root.string(), "src/api.cppm");
}

TEST(Manifest, HasLibTargetFalseForBareBinaryManifest) {
    // No [targets.*] declared → parse_string leaves targets empty.
    // load() would later infer a bin/lib from sources, but parse_string
    // alone leaves it bare; either way no lib target.
    constexpr auto src = R"(
[package]
name    = "mcpp"
version = "0.0.2"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_FALSE(mcpp::manifest::has_lib_target(*m));
}

TEST(Manifest, ParsesPerTargetFlagsAndRequiredFeatures) {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.server]
kind     = "bin"
main     = "src/server.cpp"
defines  = ["BUILD_SERVER=1", "PORT=8080"]
cxxflags = ["-fno-exceptions"]
cflags   = ["-DPURE_C"]
required_features = ["server"]
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->targets.size(), 1u);
    auto& t = m->targets[0];
    ASSERT_EQ(t.defines.size(), 2u);
    EXPECT_EQ(t.defines[0], "BUILD_SERVER=1");
    EXPECT_EQ(t.defines[1], "PORT=8080");
    ASSERT_EQ(t.cxxflags.size(), 1u);
    EXPECT_EQ(t.cxxflags[0], "-fno-exceptions");
    ASSERT_EQ(t.cflags.size(), 1u);
    EXPECT_EQ(t.cflags[0], "-DPURE_C");
    ASSERT_EQ(t.requiredFeatures.size(), 1u);
    EXPECT_EQ(t.requiredFeatures[0], "server");
    EXPECT_TRUE(m->schemaWarnings.empty());
}

TEST(Manifest, WarnsOnUnsupportedTargetKey) {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind   = "bin"
main   = "src/main.cpp"
cxxfalgs = ["-DTYPO"]
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->schemaWarnings.size(), 1u);
    EXPECT_NE(m->schemaWarnings[0].find("cxxfalgs"), std::string::npos);
    EXPECT_NE(m->schemaWarnings[0].find("unsupported key"), std::string::npos);
}

TEST(Manifest, RejectsStdFlagInTargetCxxflags) {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind     = "bin"
main     = "src/main.cpp"
cxxflags = ["-std=c++20"]
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("[package].standard"), std::string::npos);
}

TEST(ListXpkgVersions, IgnoresCommentedEntries) {
    constexpr auto src = R"(
package = {
    xpm = {
        linux = {
            -- ["1.2.3"] = { ... },   -- this is a comment
            ["0.1.0"] = { url = "u" },
        },
    },
}
)";
    auto v = mcpp::manifest::list_xpkg_versions(src, "linux");
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], "0.1.0");
}
