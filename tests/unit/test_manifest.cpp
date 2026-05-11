#include <gtest/gtest.h>

import std;
import mcpp.manifest;

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
    EXPECT_EQ(m->buildConfig.cStandard, "c11");
}

TEST(SynthesizeFromXpkgLua, CflagsCxxflagsAndCStandard) {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "tinyc",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources    = { "*/src/*.c" },
        cflags     = { "-Wall", "-Dunused" },
        cxxflags   = { "-Wextra" },
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
    EXPECT_EQ(m->buildConfig.cStandard, "c11");
    ASSERT_EQ(m->modules.sources.size(), 1u);
    EXPECT_EQ(m->modules.sources[0], "*/src/*.c");
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
    EXPECT_EQ(root.string(), "src/tinyhttps.cppm");
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
    EXPECT_EQ(root.string(), "src/gtest.cppm");
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
