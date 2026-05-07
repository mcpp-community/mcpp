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
