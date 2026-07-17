#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.platform;

TEST(Manifest, CppFlyStandard) {
    // standard = "c++fly": latest level + all experimental gates (design
    // 2026-07-14-std-features-experimental-gate-design.md §5.1).
    auto cfg = mcpp::manifest::normalize_cpp_standard("c++fly");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->canonical, "c++fly");
    EXPECT_EQ(cfg->level, 1000);          // above c++latest's 999
    EXPECT_TRUE(cfg->experimental);
    EXPECT_FALSE(mcpp::manifest::normalize_cpp_standard("c++latest")->experimental);
    EXPECT_FALSE(mcpp::manifest::normalize_cpp_standard("c++26")->experimental);
    auto bad = mcpp::manifest::normalize_cpp_standard("c++flyy");
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().find("c++fly"), std::string::npos);  // listed in the allow-list message
}

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

// Feature System v2 Stage 1: a [features] entry may be a TABLE carrying
// package-owned `defines` (and `implies`), while the array shorthand keeps
// meaning "implied features". See
// .agents/docs/2026-06-29-feature-capability-model-design.md.
TEST(Manifest, FeatureTableFormDefinesAndImplies) {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[targets.x]
kind = "lib"
[features]
default  = ["base"]
base     = []
accel    = { defines = ["APP_ACCEL=1", "APP_FAST"], implies = ["base"] }
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();

    // Array shorthand still registers an implied-feature list.
    ASSERT_TRUE(m->featuresMap.contains("default"));
    ASSERT_EQ(m->featuresMap["default"].size(), 1u);
    EXPECT_EQ(m->featuresMap["default"][0], "base");

    // Table form: `implies` flows into featuresMap, `defines` into featureDefines.
    ASSERT_TRUE(m->featuresMap.contains("accel"));
    ASSERT_EQ(m->featuresMap["accel"].size(), 1u);
    EXPECT_EQ(m->featuresMap["accel"][0], "base");

    ASSERT_TRUE(m->buildConfig.featureDefines.contains("accel"));
    ASSERT_EQ(m->buildConfig.featureDefines["accel"].size(), 2u);
    EXPECT_EQ(m->buildConfig.featureDefines["accel"][0], "APP_ACCEL=1");
    EXPECT_EQ(m->buildConfig.featureDefines["accel"][1], "APP_FAST");

    // A feature with no defines contributes no featureDefines entry.
    EXPECT_FALSE(m->buildConfig.featureDefines.contains("base"));
}

// Feature System v2 Stage 3: capabilities. A feature may `requires`/`provides`
// abstract capabilities; the package may `provides` them at package level; and
// the root [capabilities] table pins providers.
TEST(Manifest, CapabilitiesProvidesRequiresAndPins) {
    constexpr auto src = R"(
[package]
name     = "x"
version  = "0.1.0"
provides = ["blas"]
[targets.x]
kind = "lib"
[features]
default     = []
use_blas    = { requires = ["blas"] }
provide_lap = { provides = ["lapack"] }
[capabilities]
blas = "openblas"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();

    ASSERT_EQ(m->provides.size(), 1u);
    EXPECT_EQ(m->provides[0], "blas");

    ASSERT_TRUE(m->featureRequires.contains("use_blas"));
    ASSERT_EQ(m->featureRequires["use_blas"].size(), 1u);
    EXPECT_EQ(m->featureRequires["use_blas"][0], "blas");

    ASSERT_TRUE(m->featureProvides.contains("provide_lap"));
    EXPECT_EQ(m->featureProvides["provide_lap"][0], "lapack");

    ASSERT_TRUE(m->capabilityPins.contains("blas"));
    EXPECT_EQ(m->capabilityPins["blas"], "openblas");
}

// The Lua descriptor surface (index packages) parses package-level `provides`
// and feature-scoped `requires`/`provides`/`defines`.
TEST(SynthesizeFromXpkgLua, CapabilitiesAndFeatureDefines) {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "compat.eigen",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources  = { "*/anchor.c" },
        provides = { "blas" },
        targets  = { ["eigen"] = { kind = "lib" } },
        features = {
            ["use_blas"]   = { defines = { "EIGEN_USE_BLAS" }, requires = { "blas" } },
            ["eigen_blas"] = { sources = { "*/blas/*.cpp" },   provides = { "blas" } },
        },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "compat.eigen", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();

    ASSERT_EQ(m->provides.size(), 1u);
    EXPECT_EQ(m->provides[0], "blas");

    ASSERT_TRUE(m->buildConfig.featureDefines.contains("use_blas"));
    EXPECT_EQ(m->buildConfig.featureDefines["use_blas"][0], "EIGEN_USE_BLAS");
    ASSERT_TRUE(m->featureRequires.contains("use_blas"));
    EXPECT_EQ(m->featureRequires["use_blas"][0], "blas");
    ASSERT_TRUE(m->featureProvides.contains("eigen_blas"));
    EXPECT_EQ(m->featureProvides["eigen_blas"][0], "blas");
    // sources still gated as before.
    ASSERT_TRUE(m->buildConfig.featureSources.contains("eigen_blas"));
}

// Feature System v2 Stage 2a: optional deps activated by a feature.
// TOML surface uses a dedicated [feature-deps.<name>] section.
TEST(Manifest, FeatureDepsTomlSection) {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[targets.x]
kind = "lib"
[features]
default = []
backend = []
[feature-deps.backend]
zlib = "1.3.x"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_TRUE(m->featureDeps.contains("backend"));
    EXPECT_EQ(m->featureDeps["backend"].size(), 1u);
    // The optional dep is NOT in the always-on dependency set.
    EXPECT_FALSE(m->dependencies.contains("zlib"));
}

// Lua descriptor surface: a feature carries `deps` (nested table) + `implies`.
TEST(SynthesizeFromXpkgLua, FeatureDepsAndImplies) {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "compat.eigen",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/anchor.c" },
        targets = { ["eigen"] = { kind = "lib" } },
        features = {
            ["use_blas"]         = { defines = { "EIGEN_USE_BLAS" }, requires = { "blas" } },
            ["backend-openblas"] = { implies = { "use_blas" }, deps = { ["compat.openblas"] = "0.3.x" } },
        },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "compat.eigen", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    // implies recorded in featuresMap
    ASSERT_TRUE(m->featuresMap.contains("backend-openblas"));
    ASSERT_EQ(m->featuresMap["backend-openblas"].size(), 1u);
    EXPECT_EQ(m->featuresMap["backend-openblas"][0], "use_blas");
    // deps recorded in featureDeps, NOT in the always-on dependency set
    ASSERT_TRUE(m->featureDeps.contains("backend-openblas"));
    EXPECT_EQ(m->featureDeps["backend-openblas"].size(), 1u);
    EXPECT_TRUE(m->dependencies.empty());
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

TEST(SynthesizeFromXpkgLua, FeatureGatedSources) {
    // gtest-style: gtest_main.cc listed in base `sources` (old-mcpp compat) AND
    // under the `main` feature → featureSources records it; the feature is
    // registered in featuresMap. prepare_build later gates it (off by default).
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "gtestlike",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources  = { "*/src/all.cc", "*/src/main.cc" },
        targets  = { ["gtestlike"] = { kind = "lib" } },
        features = {
            ["main"] = { sources = { "*/src/main.cc" } },
        },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "gtestlike", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    // base sources keep both (old mcpp ignores `features` → no regression)
    ASSERT_EQ(m->buildConfig.sources.size(), 2u);
    // the `main` feature is registered + carries its gated source
    ASSERT_TRUE(m->featuresMap.contains("main"));
    ASSERT_TRUE(m->buildConfig.featureSources.contains("main"));
    ASSERT_EQ(m->buildConfig.featureSources.at("main").size(), 1u);
    EXPECT_EQ(m->buildConfig.featureSources.at("main")[0], "*/src/main.cc");
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

// ─── xpkg_lua_identity_matches — descriptor identity gate ───────────
//
// Regression cover for the compat.zlib vs upstream bare zlib.lua collision:
// a file found by a candidate filename must DECLARE the requested package.

namespace {
constexpr std::string_view kCompatZlibLua = R"(
package = {
    namespace   = "compat",
    name        = "compat.zlib",
    version     = "1.3.2",
    mcpp = {
        sources = { "*.c" },
    },
}
)";

// Upstream xim zlib: declares a bare name and NO namespace, no mcpp block.
constexpr std::string_view kUpstreamZlibLua = R"(
package = {
    name = "zlib",
    description = "A massively spiffy yet delicately unobtrusive compression library",
}
)";
}  // namespace

TEST(XpkgIdentity, CompatDescriptorMatchesCompatRequest) {
    EXPECT_TRUE(mcpp::manifest::xpkg_lua_identity_matches(
        kCompatZlibLua, "compat", "zlib"));
}

TEST(XpkgIdentity, UpstreamBareZlibDoesNotMatchCompatRequest) {
    // The crux of the incident: a foreign bare `zlib.lua` must be rejected for
    // a `compat.zlib` request, no matter that the candidate filename matched.
    EXPECT_FALSE(mcpp::manifest::xpkg_lua_identity_matches(
        kUpstreamZlibLua, "compat", "zlib"));
}

TEST(XpkgIdentity, DescriptorDeclaringNamespaceMatchesOnlyThatNamespace) {
    // When a descriptor DECLARES its namespace, it matches that namespace and no
    // other. (Index-owned namespace for *no-namespace* descriptors — attributing
    // a bare `zlib.lua` to its owning index as `(xim, zlib)` — is deferred to the
    // §4.1 follow-up, since the content-only gate cannot know a file's index.)
    constexpr std::string_view ximZlib =
        R"(package = { namespace = "xim", name = "zlib" })";
    EXPECT_TRUE(mcpp::manifest::xpkg_lua_identity_matches(
        ximZlib, "xim", "zlib", /*allowLegacyBareDefault=*/false));
    EXPECT_FALSE(mcpp::manifest::xpkg_lua_identity_matches(
        ximZlib, "compat", "zlib"));
}

TEST(XpkgIdentity, NoDeclaredNameIsAcceptedLeniently) {
    // A descriptor without a package.name cannot be verified → accepted.
    constexpr std::string_view noName = R"(package = { version = "1.0.0" })";
    EXPECT_TRUE(mcpp::manifest::xpkg_lua_identity_matches(noName, "compat", "zlib"));
}

TEST(XpkgIdentity, DefaultNamespaceBareNameGatedByFlag) {
    // Default-namespace (mcpplibs) bare-named legacy descriptor: accepted only
    // when the legacy-bare flag is on.
    constexpr std::string_view bareDefault = R"(package = { name = "cmdline" })";
    EXPECT_TRUE(mcpp::manifest::xpkg_lua_identity_matches(
        bareDefault, "mcpplibs", "cmdline", /*allowLegacyBareDefault=*/true));
    EXPECT_FALSE(mcpp::manifest::xpkg_lua_identity_matches(
        bareDefault, "mcpplibs", "cmdline", /*allowLegacyBareDefault=*/false));
}

TEST(XpkgIdentity, EmptyNamespaceDiscoveryMatchesNamespacedDescriptor) {
    // `mcpp new --template llmapi` reads with an empty (unknown) namespace and
    // derives the real namespace from the file. A namespaced descriptor must
    // still be discoverable by its short name.
    constexpr std::string_view llmapi = R"(
package = { namespace = "mcpplibs", name = "mcpplibs.llmapi", version = "0.2.8" }
)";
    EXPECT_TRUE(mcpp::manifest::xpkg_lua_identity_matches(llmapi, "", "llmapi"));
    // ...but it is NOT a match for a different short name.
    EXPECT_FALSE(mcpp::manifest::xpkg_lua_identity_matches(llmapi, "", "zlib"));
}

TEST(XpkgIdentity, DefaultNamespaceRequestMatchesCompatAlias) {
    // Regression for the CI break: the dev-dep `gtest` is a bare/default-namespace
    // request, but the descriptor is `compat.gtest` (namespace="compat"). A
    // default-namespace request must accept its compat alias.
    constexpr std::string_view compatGtest =
        R"(package = { namespace = "compat", name = "compat.gtest", version = "1.15.2" })";
    EXPECT_TRUE(mcpp::manifest::xpkg_lua_identity_matches(
        compatGtest, "mcpplibs", "gtest"));
    EXPECT_TRUE(mcpp::manifest::xpkg_lua_identity_matches(
        compatGtest, "mcpplibs", "gtest", /*allowLegacyBareDefault=*/false));
    // But a different default-ns name does not match it.
    EXPECT_FALSE(mcpp::manifest::xpkg_lua_identity_matches(
        compatGtest, "mcpplibs", "zlib"));
}

// ─── canonical_xpkg_identity — the unified (ns, name) model (§4.2) ───
//
// Identity is a 2-tuple: ns is a hierarchical namespace path, name is a single
// atomic segment. Every surface spelling normalizes to this tuple.

namespace {
mcpp::manifest::XpkgIdentity id(std::string_view ns, std::string_view name,
                                std::string_view indexNs = {}) {
    return mcpp::manifest::canonical_xpkg_identity(ns, name, indexNs);
}
mcpp::manifest::XpkgIdentity want(std::string n, std::string s) {
    return mcpp::manifest::XpkgIdentity{std::move(n), std::move(s)};
}
}  // namespace

TEST(CanonicalIdentity, PrefixEmbeddedNameCollapses) {
    // ns=compat, name=compat.zlib  →  (compat, zlib)
    EXPECT_EQ(id("compat", "compat.zlib"), want("compat", "zlib"));
}

TEST(CanonicalIdentity, BareNameCombinesWithNamespace) {
    // ns=mcpplibs, name=cmdline  →  (mcpplibs, cmdline)
    EXPECT_EQ(id("mcpplibs", "cmdline"), want("mcpplibs", "cmdline"));
}

TEST(CanonicalIdentity, AlreadyQualifiedNameIsIdempotent) {
    EXPECT_EQ(id("mcpplibs", "mcpplibs.cmdline"), want("mcpplibs", "cmdline"));
}

TEST(CanonicalIdentity, NoNamespaceInheritsOwningIndex) {
    // ns=∅, name=zlib, index=xim  →  (xim, zlib)
    EXPECT_EQ(id("", "zlib", "xim"), want("xim", "zlib"));
    EXPECT_EQ(id("", "tinycfg", "local-dev"), want("local-dev", "tinycfg"));
}

TEST(CanonicalIdentity, DeclaredNamespaceWinsOverIndexDefault) {
    EXPECT_EQ(id("compat", "compat.zlib", "xim"), want("compat", "zlib"));
}

TEST(CanonicalIdentity, DottedNameWithNoNamespaceSplitsOnLastDot) {
    // ns=∅, name=a.b  →  (a, b)
    EXPECT_EQ(id("", "a.b"), want("a", "b"));
    EXPECT_EQ(id("", "x.y.z"), want("x.y", "z"));
}

TEST(CanonicalIdentity, HierarchicalNamespaceIsSupported) {
    // ns=a.b, name=c  →  (a.b, c) ; name is the single trailing segment.
    EXPECT_EQ(id("a.b", "c"), want("a.b", "c"));
    EXPECT_EQ(id("mcpplibs.capi", "lua"), want("mcpplibs.capi", "lua"));
    // Nested + prefix-embedded forms both land on the same tuple.
    EXPECT_EQ(id("mcpplibs", "mcpplibs.capi.lua"), want("mcpplibs.capi", "lua"));
    EXPECT_EQ(id("mcpplibs.capi", "mcpplibs.capi.lua"),
              want("mcpplibs.capi", "lua"));
}

TEST(CanonicalIdentity, BareNameNoNamespaceNoIndexStaysRootless) {
    // The incident villain: a foreign bare descriptor with nothing to anchor it.
    EXPECT_EQ(id("", "zlib"), want("", "zlib"));
}

TEST(CanonicalIdentity, FromLuaReadsDeclaredFields) {
    constexpr std::string_view lua =
        R"(package = { namespace = "compat", name = "compat.zlib" })";
    EXPECT_EQ(mcpp::manifest::canonical_xpkg_identity_from_lua(lua),
              want("compat", "zlib"));
    // No-namespace descriptor + owning index.
    constexpr std::string_view bare = R"(package = { name = "tinycfg" })";
    EXPECT_EQ(mcpp::manifest::canonical_xpkg_identity_from_lua(bare, "local-dev"),
              want("local-dev", "tinycfg"));
    // No declared name → empty identity.
    constexpr std::string_view noName = R"(package = { version = "1.0" })";
    EXPECT_EQ(mcpp::manifest::canonical_xpkg_identity_from_lua(noName),
              want("", ""));
}

// ── Descriptor grammar v2: Lua long-bracket strings + block comments ──
// Design: .agents/docs/2026-07-08-index-version-semantics-and-descriptor-grammar-design.md (D1)

TEST(XpkgLongBracket, GeneratedFilesMultiline) {
    // Content carries braces, quotes, an embedded `]]` (level-2 brackets),
    // and a leading newline that Lua semantics strip.
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "lbtest",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "mcpp_generated/gen.cc" },
        generated_files = {
            ["mcpp_generated/gen.cc"] = [==[
export module gen;
int f() { return "x]]y"[0]; }
]==],
        },
        targets = { ["gen"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "lbtest", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_TRUE(m->buildConfig.generatedFiles.contains("mcpp_generated/gen.cc"));
    EXPECT_EQ(m->buildConfig.generatedFiles["mcpp_generated/gen.cc"],
              "export module gen;\nint f() { return \"x]]y\"[0]; }\n");
}

TEST(XpkgLongBracket, BlockCommentWithBracesAndFakeMcpp) {
    // A block comment containing braces and the literal text `mcpp = {`
    // must neither corrupt the structural depth count nor false-match
    // the segment locator.
    constexpr auto lua = R"(
--[==[
decoy: mcpp = { sources = { "WRONG" } } }}}}
]==]
package = {
    spec = "1",
    name = "cmt",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        --[[ inline block comment { with braces } ]]
        sources = { [[src/*.cpp]] },
        targets = { ["cmt"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "cmt", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->modules.sources.size(), 1u);
    EXPECT_EQ(m->modules.sources[0], "src/*.cpp");
}

TEST(XpkgLongBracket, NoLeadingNewlineKeptVerbatim) {
    // Without a newline after the opening bracket, content starts
    // immediately; CRLF right after the bracket is stripped as one break.
    constexpr auto lua =
        "package = {\n"
        "    spec = \"1\",\n"
        "    name = \"nl\",\n"
        "    xpm  = { linux = { [\"1.0.0\"] = { url = \"u\", sha256 = \"h\" } } },\n"
        "    mcpp = {\n"
        "        sources = { \"g.cc\" },\n"
        "        generated_files = { [\"g.cc\"] = [[inline]], [\"h.cc\"] = [[\r\ncrlf]] },\n"
        "        targets = { [\"nl\"] = { kind = \"lib\" } },\n"
        "    },\n"
        "}\n";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "nl", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_EQ(m->buildConfig.generatedFiles["g.cc"], "inline");
    EXPECT_EQ(m->buildConfig.generatedFiles["h.cc"], "crlf");
}

TEST(XpkgLongBracket, UnknownKeysRecordedSchemaAccepted) {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "uk",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        schema  = "0.1",
        sources = { "a.c" },
        bogus_key = "x",
        future_table = { nested = { 1, 2 } },
        targets = { ["uk"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "uk", "1.0.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->xpkgUnknownKeys.size(), 2u);
    EXPECT_EQ(m->xpkgUnknownKeys[0], "bogus_key");
    EXPECT_EQ(m->xpkgUnknownKeys[1], "future_table");
}

// ── scan_overrides: author-asserted scan results ──
// Design: .agents/docs/2026-07-08-scanner-backend-abstraction-design.md §3-pre

TEST(ScanOverrides, XpkgSegmentParses) {
    constexpr auto lua = R"(
package = {
    spec = "1",
    namespace = "fmtlib",
    name = "fmtlib.fmt",
    xpm  = { linux = { ["12.2.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources  = { "*/src/fmt.cc" },
        cxxflags = { "-DFMT_IMPORT_STD" },
        scan_overrides = {
            ["*/src/fmt.cc"] = { provides = { "fmt" }, imports = { "std" } },
        },
        targets  = { ["fmt"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "fmtlib.fmt", "12.2.0");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->modules.scanOverrides.size(), 1u);
    auto& ov = m->modules.scanOverrides.at("*/src/fmt.cc");
    ASSERT_EQ(ov.provides.size(), 1u);
    EXPECT_EQ(ov.provides[0], "fmt");
    ASSERT_EQ(ov.imports.size(), 1u);
    EXPECT_EQ(ov.imports[0], "std");
    EXPECT_TRUE(m->xpkgUnknownKeys.empty());
}

TEST(ScanOverrides, XpkgRejectsEmptyDeclaration) {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "so",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "a.cc" },
        scan_overrides = { ["a.cc"] = { } },
        targets = { ["so"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "so", "1.0.0");
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("neither provides nor imports"),
              std::string::npos) << m.error().format();
}

TEST(ScanOverrides, TomlParses) {
    constexpr auto src = R"(
[package]
name = "app"
version = "0.1.0"
[language]
standard = "c++23"
[modules]
sources = ["src/**/*.cppm", "vendor/fmt.cc"]
["scan_overrides"."vendor/fmt.cc"]
provides = ["fmt"]
imports  = ["std"]
[targets.app]
kind = "bin"
main = "src/main.cpp"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->modules.scanOverrides.size(), 1u);
    auto& ov = m->modules.scanOverrides.at("vendor/fmt.cc");
    EXPECT_EQ(ov.provides, std::vector<std::string>{"fmt"});
    EXPECT_EQ(ov.imports,  std::vector<std::string>{"std"});
}

// ── declarative source-set parity: mcpp.toml ↔ xpkg descriptor ───────────────
// One data model, two grammars (manifest.cppm): what a descriptor can express
// through `features.<f>.sources` / `generated_files`, mcpp.toml must express
// identically. This test locks the symmetry as a contract.

TEST(Manifest, FeatureSourcesParseFromToml) {
    constexpr auto src = R"(
[package]
name = "sym"
version = "1.0.0"
[features]
default = []
compiled = { sources = ["src/impl/**"], defines = ["SYM_COMPILED"] }
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_TRUE(m->buildConfig.featureSources.contains("compiled"));
    EXPECT_EQ(m->buildConfig.featureSources.at("compiled"),
              (std::vector<std::string>{"src/impl/**"}));
    ASSERT_TRUE(m->buildConfig.featureDefines.contains("compiled"));
}

TEST(Manifest, GeneratedFilesParseFromToml) {
    constexpr auto src = R"(
[package]
name = "genf"
version = "1.0.0"

[generated_files]
"src/gen/wrap.cppm" = """
module;
export module wrap;
"""
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->buildConfig.generatedFiles.size(), 1u);
    auto it = m->buildConfig.generatedFiles.find(
        std::filesystem::path("src/gen/wrap.cppm"));
    ASSERT_NE(it, m->buildConfig.generatedFiles.end());
    EXPECT_EQ(it->second, "module;\nexport module wrap;\n");
}

TEST(Manifest, GeneratedFilesRejectEscapingPaths) {
    for (auto path : { "\"../evil.cpp\"", "\"/abs/evil.cpp\"", "\"a/../../evil.cpp\"" }) {
        auto src = std::format(R"(
[package]
name = "genf"
version = "1.0.0"
[generated_files]
{} = "x"
)", path);
        auto m = mcpp::manifest::parse_string(src);
        EXPECT_FALSE(m.has_value()) << "path accepted: " << path;
    }
}

TEST(Manifest, TomlAndXpkgFeatureSourcesAreSymmetric) {
    constexpr auto toml = R"(
[package]
name = "sym"
version = "1.0.0"
[features]
compiled = { sources = ["src/impl/**"], defines = ["SYM_COMPILED"] }
[generated_files]
"src/gen/wrap.cppm" = "module;\nexport module wrap;\n"
)";
    constexpr auto lua = R"(
package = { name = "sym" }
mcpp = {
    sources = { "src/**/*.cpp" },
    features = {
        compiled = { sources = { "src/impl/**" }, defines = { "SYM_COMPILED" } },
    },
    generated_files = {
        ["src/gen/wrap.cppm"] = "module;\nexport module wrap;\n",
    },
}
)";
    auto mt = mcpp::manifest::parse_string(toml);
    ASSERT_TRUE(mt.has_value()) << mt.error().format();
    auto mx = mcpp::manifest::synthesize_from_xpkg_lua(lua, "sym", "1.0.0");
    ASSERT_TRUE(mx.has_value()) << mx.error().format();

    EXPECT_EQ(mt->buildConfig.featureSources, mx->buildConfig.featureSources);
    EXPECT_EQ(mt->buildConfig.featureDefines, mx->buildConfig.featureDefines);
    EXPECT_EQ(mt->buildConfig.generatedFiles, mx->buildConfig.generatedFiles);
}

TEST(Manifest, CfgConditionalSourcesParseFromToml) {
    constexpr auto src = R"(
[package]
name = "condsrc"
version = "1.0.0"

[target.'cfg(arch = "x86_64")'.build]
sources  = ["src/x86/**/*.asm", "!src/x86/legacy/**"]
cxxflags = ["-DHAVE_X86=1"]
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_EQ(m->conditionalConfigs.size(), 1u);
    auto& cc = m->conditionalConfigs[0];
    EXPECT_EQ(cc.predicate, "cfg(arch = \"x86_64\")");
    EXPECT_EQ(cc.sources, (std::vector<std::string>{
        "src/x86/**/*.asm", "!src/x86/legacy/**"}));
    EXPECT_EQ(cc.cxxflags, (std::vector<std::string>{"-DHAVE_X86=1"}));
}

TEST(Manifest, TargetCfgParsesFromXpkgSymmetrically) {
    constexpr auto lua = R"(
package = { name = "condsrc" }
mcpp = {
    sources = { "src/**/*.c" },
    target_cfg = {
        ['cfg(arch = "x86_64")'] = {
            sources  = { "src/x86/**/*.asm" },
            cxxflags = { "-DHAVE_X86=1" },
        },
        ['cfg(windows)'] = {
            ldflags = { "-lws2_32" },
        },
    },
}
)";
    auto mx = mcpp::manifest::synthesize_from_xpkg_lua(lua, "condsrc", "1.0.0");
    ASSERT_TRUE(mx.has_value()) << mx.error().format();
    ASSERT_EQ(mx->conditionalConfigs.size(), 2u);
    EXPECT_EQ(mx->conditionalConfigs[0].predicate, "cfg(arch = \"x86_64\")");
    EXPECT_EQ(mx->conditionalConfigs[0].sources,
              (std::vector<std::string>{"src/x86/**/*.asm"}));
    EXPECT_EQ(mx->conditionalConfigs[1].predicate, "cfg(windows)");
    EXPECT_EQ(mx->conditionalConfigs[1].ldflags,
              (std::vector<std::string>{"-lws2_32"}));
}

TEST(Manifest, PerGlobFlagsParseOrderedFromToml) {
    constexpr auto src = R"(
[package]
name = "globf"
version = "1.0.0"

[build]
flags = [
  { glob = "third_party/**", cflags = ["-w"], cxxflags = ["-w"] },
  { glob = "src/simd/**/*.avx2.cpp", cxxflags = ["-mavx2"], defines = ["HAVE_AVX2"] },
  { glob = "src/x86/**/*.asm", asmflags = ["-DPIC"] },
]
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    auto& gfs = m->buildConfig.globFlags;
    ASSERT_EQ(gfs.size(), 3u);
    // Declaration order IS the data (override semantics ride on it).
    EXPECT_EQ(gfs[0].glob, "third_party/**");
    EXPECT_EQ(gfs[0].cxxflags, (std::vector<std::string>{"-w"}));
    EXPECT_EQ(gfs[1].glob, "src/simd/**/*.avx2.cpp");
    EXPECT_EQ(gfs[1].defines, (std::vector<std::string>{"HAVE_AVX2"}));
    EXPECT_EQ(gfs[2].asmflags, (std::vector<std::string>{"-DPIC"}));
}

TEST(Manifest, PerGlobFlagsRejectUnknownKeysAndMissingGlob) {
    auto bad1 = mcpp::manifest::parse_string(R"(
[package]
name = "globf"
version = "1.0.0"
[build]
flags = [{ glob = "src/**", ldflags = ["-lfoo"] }]
)");
    EXPECT_FALSE(bad1.has_value());   // ldflags is not per-TU — closed grammar

    auto bad2 = mcpp::manifest::parse_string(R"(
[package]
name = "globf"
version = "1.0.0"
[build]
flags = [{ cxxflags = ["-w"] }]
)");
    EXPECT_FALSE(bad2.has_value());   // missing glob
}

TEST(Manifest, PerGlobFlagsParseFromXpkgSymmetrically) {
    constexpr auto lua = R"(
package = { name = "globf" }
mcpp = {
    sources = { "src/**/*.cpp" },
    flags = {
        { glob = "third_party/**", cxxflags = { "-w" } },
        { glob = "src/simd/**", cxxflags = { "-mavx2" }, defines = { "HAVE_AVX2" } },
    },
}
)";
    auto mx = mcpp::manifest::synthesize_from_xpkg_lua(lua, "globf", "1.0.0");
    ASSERT_TRUE(mx.has_value()) << mx.error().format();
    ASSERT_EQ(mx->buildConfig.globFlags.size(), 2u);
    EXPECT_EQ(mx->buildConfig.globFlags[0].glob, "third_party/**");
    EXPECT_EQ(mx->buildConfig.globFlags[1].cxxflags,
              (std::vector<std::string>{"-mavx2"}));
    EXPECT_EQ(mx->buildConfig.globFlags[1].defines,
              (std::vector<std::string>{"HAVE_AVX2"}));
}
