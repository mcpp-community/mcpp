// Sources (mcpp#755): the manifest keys that state where a payload or a
// toolchain comes from, the protocol-15 directives a build program states
// sources with, and the override checks of the payload unification.

#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.config;
import mcpp.build.build_program;
import mcpp.libs.toml;
import mcpp.build.directives;
import mcpp.toolchain.dialect;
import mcpp.toolchain.registry;
import mcpp.provider.address_set;

namespace dirs = mcpp::build::directives;
namespace addrset = mcpp::provider::addrset;

namespace {

dirs::Directives parse_directives(std::string_view text) {
    dirs::Directives d;
    const auto root = (std::filesystem::current_path() / "pkg").lexically_normal();
    dirs::accept_output(d, mcpp::toolchain::gnu_dialect(), root, text);
    return d;
}

std::expected<mcpp::manifest::Manifest, mcpp::manifest::ManifestError>
manifest(std::string_view body) {
    return mcpp::manifest::parse_string(std::format(R"(
[package]
name = "app"
version = "0.1.0"
{})", body));
}

} // namespace

// ── [xlings.overrides] ──────────────────────────────────────────────────────

TEST(Sources, OverridePathAndTableFormsParse) {
    auto m = manifest(R"(
[xlings.overrides]
"xim:cmake" = "/usr/bin/cmake"
vcpkg       = { root = "/opt/vcpkg" }
"xim:slang" = { program = "slangc", version = "2026.14.1" }
)");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    using K = mcpp::manifest::XlingsOverride::Kind;
    const auto& o = m->xlings.overrides;
    ASSERT_EQ(o.size(), 3u);
    EXPECT_EQ(o.at("xim:cmake").kind, K::Path);
    EXPECT_EQ(o.at("xim:cmake").value, "/usr/bin/cmake");
    // A key without a namespace names the `xim` package, as everywhere else.
    EXPECT_EQ(o.at("xim:vcpkg").kind, K::Root);
    EXPECT_EQ(o.at("xim:slang").kind, K::Program);
    EXPECT_EQ(o.at("xim:slang").version, "2026.14.1");
    EXPECT_GT(o.at("xim:cmake").line, 0);
}

TEST(Sources, OverrideKeyWithAVersionIsRefused) {
    auto m = manifest(R"(
[xlings.overrides]
"xim:cmake@3.31" = "/usr/bin/cmake"
)");
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("names a version"), std::string::npos) << m.error().message;
}

TEST(Sources, OverrideNamingBothProgramAndRootIsRefused) {
    auto m = manifest(R"(
[xlings.overrides]
cmake = { program = "/usr/bin/cmake", root = "/usr" }
)");
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("exactly one of"), std::string::npos) << m.error().message;
}

TEST(Sources, OverrideUnknownKeyIsRefused) {
    auto m = manifest(R"(
[xlings.overrides]
cmake = { programme = "/usr/bin/cmake" }
)");
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("programme"), std::string::npos) << m.error().message;
}

TEST(Sources, OverrideUnderATargetSelectorParses) {
    auto m = manifest(R"(
[target.'cfg(os = "linux")'.xlings.overrides]
"xim:cmake" = "/usr/bin/cmake"
)");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    ASSERT_FALSE(m->conditionalConfigs.empty());
    bool found = false;
    for (auto const& cc : m->conditionalConfigs)
        if (cc.xlings.overrides.contains("xim:cmake")) found = true;
    EXPECT_TRUE(found);
}

// ── provision = "on-request" ────────────────────────────────────────────────

TEST(Sources, ProvisionOnRequestIsRecordedOnBothTables) {
    auto m = manifest(R"(
[features]
tools = {}
[xlings.workspace]
"xim:cmake" = { version = ">=3.31", provision = "on-request" }
"xim:ninja" = { version = "1.12", provision = "eager" }
[feature-xlings.tools]
"xim:slang" = { version = "", provision = "on-request", when = "build" }
)");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_TRUE(m->xlings.on_request("xim:cmake@>=3.31"));
    EXPECT_FALSE(m->xlings.on_request("xim:ninja@1.12"));
    EXPECT_TRUE(m->xlings.on_request("xim:slang"));
    EXPECT_EQ(m->xlings.when_of("xim:slang"), mcpp::manifest::ToolWhen::Build);
}

TEST(Sources, ProvisionUnknownModeIsRefused) {
    auto m = manifest(R"(
[xlings.workspace]
"xim:cmake" = { version = "", provision = "lazy" }
)");
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("on-request"), std::string::npos) << m.error().message;
}

// ── [toolchain] tables ──────────────────────────────────────────────────────

TEST(Sources, ToolchainTableNamesAPath) {
    auto m = manifest(R"(
[toolchain]
default   = { path = "/opt/llvm", launcher = "ccache", tools = { ld = "/opt/lld/bin/ld.lld" } }
bootstrap = "llvm@22.1.8"
)");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_EQ(m->toolchain.for_platform("linux").value_or(""), "path:/opt/llvm");
    auto* lt = m->toolchain.local_for("linux");
    ASSERT_NE(lt, nullptr);
    EXPECT_EQ(lt->launcher, "ccache");
    ASSERT_EQ(lt->tools.size(), 1u);
    EXPECT_EQ(lt->tools[0].first, "ld");
    EXPECT_EQ(m->toolchain.bootstrap, "llvm@22.1.8");
    // `bootstrap` is not a platform.
    EXPECT_FALSE(m->toolchain.byPlatform.contains("bootstrap"));
}

TEST(Sources, ToolchainConfigureIsAloneInItsTable) {
    auto ok = manifest(R"(
[toolchain]
default = { configure = "build.mcpp" }
)");
    ASSERT_TRUE(ok.has_value()) << ok.error().format();
    EXPECT_EQ(ok->toolchain.for_platform("macos").value_or(""), "configure:build.mcpp");
    auto both = manifest(R"(
[toolchain]
default = { configure = "build.mcpp", path = "/opt/llvm" }
)");
    ASSERT_FALSE(both.has_value());
    auto other = manifest(R"(
[toolchain]
default = { configure = "cmake" }
)");
    ASSERT_FALSE(other.has_value());
    EXPECT_NE(other.error().message.find("build.mcpp"), std::string::npos) << other.error().message;
}

TEST(Sources, ToolchainTableWithoutPathIsRefused) {
    auto m = manifest(R"(
[toolchain]
linux = { prefix = "aarch64-none-linux-gnu-" }
)");
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("path"), std::string::npos) << m.error().message;
}

TEST(Sources, ToolchainUnknownToolRoleIsRefused) {
    auto m = manifest(R"(
[toolchain]
default = { path = "/opt/llvm", tools = { linker = "/x" } }
)");
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("linker"), std::string::npos) << m.error().message;
}

// ── Protocol-15 directives ──────────────────────────────────────────────────

TEST(Sources, SourceDirectivesFoldIntoTheBuildConfig) {
    auto d = parse_directives(
        "mcpp:protocol=15\n"
        "mcpp:decision=tool:mcpp.deps.cmake:cmake\tchoice\t/usr/bin/cmake\t/p/build.mcpp\t9\txim:cmake\n"
        "mcpp:xpkg-request=xim:cmake\n"
        "mcpp:toolchain=path=/opt/llvm\n"
        "mcpp:toolchain=origin=/p/build.mcpp:4\n");
    EXPECT_FALSE(dirs::protocol_error(d).has_value());
    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    ASSERT_EQ(m.buildConfig.toolDecisions.size(), 1u);
    EXPECT_TRUE(m.buildConfig.toolDecisions[0].starts_with("tool:mcpp.deps.cmake:cmake\tchoice"));
    ASSERT_EQ(m.buildConfig.xpkgRequests.size(), 1u);
    EXPECT_EQ(m.buildConfig.xpkgRequests[0], "xim:cmake");
    ASSERT_EQ(m.buildConfig.toolchainStatement.size(), 2u);
    EXPECT_EQ(m.buildConfig.toolchainStatement[0], "path=/opt/llvm");
}

TEST(Sources, SourceDirectivesAreProtocolFifteen) {
    for (auto wire : {"decision", "xpkg-request", "toolchain"}) {
        auto def = dirs::find_by_wire(wire);
        ASSERT_NE(def, nullptr) << wire;
        EXPECT_EQ(def->sinceProtocol, 15) << wire;
        EXPECT_FALSE(def->tag.empty()) << wire;
    }
}

// ── Override checks in the unification ─────────────────────────────────────

TEST(Sources, OverrideVersionIsCheckedAgainstRequirements) {
    std::vector<addrset::Claim> claims{
        {"xim:cmake@>=3.31", "mcpp:plugins", 1},
        {"xim:cmake", "this project", 0},
    };
    EXPECT_FALSE(addrset::override_violation(claims, "xim:cmake", "3.31.6", "[xlings.overrides]"));
    auto bad = addrset::override_violation(claims, "xim:cmake", "3.28.3", "[xlings.overrides]");
    ASSERT_TRUE(bad.has_value());
    EXPECT_NE(bad->find(">=3.31"), std::string::npos) << *bad;
    EXPECT_NE(bad->find("mcpp:plugins"), std::string::npos) << *bad;
    // No stated version: nothing to compare, and the requirement is listed.
    EXPECT_FALSE(addrset::override_violation(claims, "xim:cmake", "", "[xlings.overrides]"));
    auto reqs = addrset::requirements_for(claims, "xim:cmake");
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0], ">=3.31 by mcpp:plugins");
}

// ── The response file a long compile command goes through ───────────────────

TEST(Sources, ResponseFileQuotesForTheWindowsTokenizer) {
    const std::vector<std::string> args{
        "-std=c++23",
        "/Tp C:/Program Files/x/build.mcpp",
        R"(-DNAME="v")",
        "C:\\with space\\dir\\",
    };
    const auto body = mcpp::build::response_file_body(args, /*gnuQuoting=*/false);
    EXPECT_EQ(std::ranges::count(body, '\n'), 4);
    EXPECT_NE(body.find("-std=c++23\n"), std::string::npos) << body;
    EXPECT_NE(body.find("\"/Tp C:/Program Files/x/build.mcpp\"\n"), std::string::npos) << body;
    EXPECT_NE(body.find("\"-DNAME=\\\"v\\\"\"\n"), std::string::npos) << body;
    // The run of backslashes that ends the argument is doubled, so it does not
    // escape the closing quote.
    EXPECT_NE(body.find("\"C:\\with space\\dir\\\\\"\n"), std::string::npos) << body;
}

TEST(Sources, ResponseFileKeepsBackslashesLiteralForTheGnuTokenizer) {
    // clang and GCC read a backslash as an escape, inside quotes as well as
    // outside, so a Windows path survives only when every backslash is doubled.
    // Measured with clang 22.1.8: `'-DX=a\b'` in a response file yields `X=ab`,
    // and `-DX=a\\b` yields `X=a\b`.
    const std::vector<std::string> args{
        "-fmodule-file=mcpp=D:\\a\\p\\mcpp.pcm",
        "D:\\a\\obj\\x.o",
        "/Tp D:\\a\\b c\\build.mcpp",
        R"(-DNAME="v")",
    };
    const auto body = mcpp::build::response_file_body(args, /*gnuQuoting=*/true);
    EXPECT_EQ(std::ranges::count(body, '\n'), 4);
    EXPECT_NE(body.find("-fmodule-file=mcpp=D:\\\\a\\\\p\\\\mcpp.pcm\n"), std::string::npos) << body;
    EXPECT_NE(body.find("D:\\\\a\\\\obj\\\\x.o\n"), std::string::npos) << body;
    // Whitespace is handled by quoting the whole argument; the doubling holds
    // inside the quotes too.
    EXPECT_NE(body.find("\"/Tp D:\\\\a\\\\b c\\\\build.mcpp\"\n"), std::string::npos) << body;
    // A quote of the argument's own is escaped.
    EXPECT_NE(body.find("-DNAME=\\\"v\\\"\n"), std::string::npos) << body;
}

// ── [xlings.overrides] in config.toml ───────────────────────────────────────

TEST(Sources, ConfigOverridesParseWithTheManifestsTwoShapes) {
    auto doc = mcpp::libs::toml::parse(R"(
[xlings.overrides]
"xim:cmake" = "/usr/bin/cmake"
vcpkg       = { root = "/opt/vcpkg" }
"xim:slang" = { program = "slangc", version = "2026.14.1" }
)");
    ASSERT_TRUE(doc.has_value());
    auto o = mcpp::config::parse_payload_overrides(*doc);
    ASSERT_TRUE(o.has_value()) << o.error();
    ASSERT_EQ(o->size(), 3u);
    EXPECT_EQ(o->at("xim:cmake").kind, "path");
    EXPECT_EQ(o->at("xim:cmake").value, "/usr/bin/cmake");
    // A key without a namespace names the `xim` package, as everywhere else.
    EXPECT_EQ(o->at("xim:vcpkg").kind, "root");
    EXPECT_EQ(o->at("xim:slang").kind, "program");
    EXPECT_EQ(o->at("xim:slang").version, "2026.14.1");
    EXPECT_GT(o->at("xim:cmake").line, 0);
}

TEST(Sources, ConfigOverrideWithAnUnknownKeyIsRefused) {
    auto doc = mcpp::libs::toml::parse(R"(
[xlings.overrides]
cmake = { programme = "/usr/bin/cmake" }
)");
    ASSERT_TRUE(doc.has_value());
    auto o = mcpp::config::parse_payload_overrides(*doc);
    ASSERT_FALSE(o.has_value());
    EXPECT_NE(o.error().find("programme"), std::string::npos) << o.error();
}

TEST(Sources, ConfigWithoutTheTableHasNoOverrides) {
    auto doc = mcpp::libs::toml::parse("[toolchain]\ndefault = \"gcc@16.1.0\"\n");
    ASSERT_TRUE(doc.has_value());
    auto o = mcpp::config::parse_payload_overrides(*doc);
    ASSERT_TRUE(o.has_value()) << o.error();
    EXPECT_TRUE(o->empty());
}

// ── A toolchain named by path ───────────────────────────────────────────────

TEST(Sources, PathSpecTakesTheFamilyFromTheDrivers) {
    const auto base = std::filesystem::temp_directory_path() / "mcpp-test-sources-tc";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "llvm" / "bin");
    std::filesystem::create_directories(base / "gcc" / "bin");
    std::ofstream(base / "llvm" / "bin" / "clang++") << "";
    std::ofstream(base / "gcc" / "bin" / "aarch64-none-linux-gnu-g++") << "";
    auto llvm = mcpp::toolchain::parse_toolchain_spec("path:" + (base / "llvm").string());
    ASSERT_TRUE(llvm.has_value()) << llvm.error();
    EXPECT_EQ(llvm->family, mcpp::toolchain::Family::Llvm);
    EXPECT_EQ(llvm->spec_str(), "path:" + (base / "llvm").lexically_normal().generic_string());
    auto gcc = mcpp::toolchain::parse_toolchain_spec("path:" + (base / "gcc").string());
    ASSERT_TRUE(gcc.has_value()) << gcc.error();
    EXPECT_EQ(gcc->family, mcpp::toolchain::Family::Gcc);
    auto none = mcpp::toolchain::parse_toolchain_spec("path:" + (base / "missing").string());
    EXPECT_FALSE(none.has_value());
    std::filesystem::remove_all(base);
}

// ── The engine floor read from a manifest that does not parse ───────────────

TEST(Sources, StatedMcppFloorIsReadFromPackageOnly) {
    using mcpp::manifest::stated_mcpp_floor;
    EXPECT_EQ(stated_mcpp_floor("[package]\nname = \"p\"\nmcpp = \">=2026.10.1.3\"\n"),
              "2026.10.1.3");
    // A bare version is the same statement.
    EXPECT_EQ(stated_mcpp_floor("[package]\nmcpp = \"2026.9.28.3\"\n"), "2026.9.28.3");
    // Only `[package]`: a dependency named mcpp states a dependency, not a floor.
    EXPECT_EQ(stated_mcpp_floor("[package]\nname = \"p\"\n\n[dependencies]\nmcpp = \"1.0\"\n"),
              "");
    // `[package.metadata]` is another table.
    EXPECT_EQ(stated_mcpp_floor("[package.metadata]\nmcpp = \"9.9.9.9\"\n"), "");
    // A key that merely starts with the name is not the key.
    EXPECT_EQ(stated_mcpp_floor("[package]\nmcpp_home = \"/x\"\n"), "");
    EXPECT_EQ(stated_mcpp_floor("[package]\n# mcpp = \"9.9.9.9\"\n"), "");
    EXPECT_EQ(stated_mcpp_floor(""), "");
}
