#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.project;

// The unit-level statements of a workspace plan (workspace design 2026-09-29
// §15): which member values separate two plans, what the virtual root holds,
// and where a member's products are placed. The end-to-end halves are
// tests/e2e/834_a_workspace_is_one_graph_per_configuration.sh and its
// neighbours.

namespace {

mcpp::manifest::Manifest member(std::string name) {
    mcpp::manifest::Manifest m;
    m.package.name = std::move(name);
    m.package.version = "0.1.0";
    return m;
}

mcpp::project::WorkspaceMember listed(std::string path, std::string ns, std::string name) {
    return {.memberPath = path, .dir = path, .namespace_ = std::move(ns),
            .name = std::move(name)};
}

} // namespace

// A member's own flags, sources and dependencies are attributes of its node:
// they never separate it from another member.
TEST(WorkspacePlan, PackageAttributesDoNotSeparateMembers) {
    auto a = member("a");
    auto b = member("b");
    b.buildConfig.cxxflags = {"-DONLY_B=1"};
    b.buildConfig.ldflags = {"-lm"};
    b.buildConfig.sources = {"src/**/*.cpp"};
    b.buildConfig.cStandard = "c17";
    b.dependencies["fmt"] = mcpp::manifest::DependencySpec{.version = "11.0.0"};
    EXPECT_EQ(mcpp::project::root_position_key(a), mcpp::project::root_position_key(b));
}

// What every node of one graph shares does separate them: the toolchain, the
// standard, the dialect flags, the target and the profile.
TEST(WorkspacePlan, RootPositionValuesSeparateMembers) {
    const auto ref = mcpp::project::root_position_key(member("a"));
    { auto m = member("a"); m.package.standard = "c++26";
      EXPECT_NE(mcpp::project::root_position_key(m), ref); }
    { auto m = member("a"); m.toolchain.byPlatform["default"] = "gcc@16.1.0";
      EXPECT_NE(mcpp::project::root_position_key(m), ref); }
    { auto m = member("a"); m.buildConfig.dialectCxxflags = {"-freflection"};
      EXPECT_NE(mcpp::project::root_position_key(m), ref); }
    { auto m = member("a"); m.buildConfig.target = "x86_64-linux-musl";
      EXPECT_NE(mcpp::project::root_position_key(m), ref); }
    { auto m = member("a"); m.buildConfig.defaultProfile = "release";
      EXPECT_NE(mcpp::project::root_position_key(m), ref); }
    { auto m = member("a"); m.buildConfig.cxxRuntime = "static";
      EXPECT_NE(mcpp::project::root_position_key(m), ref); }
    // `linkage` chooses the C runtime every object of the plan is compiled
    // against, so it is the plan's and not the member's.
    { auto m = member("a"); m.buildConfig.linkage = "static";
      EXPECT_NE(mcpp::project::root_position_key(m), ref); }
}

// The virtual root carries the plan's values and nothing a package owns.
TEST(WorkspacePlan, TheVirtualRootHoldsNoPackageContent) {
    mcpp::manifest::Manifest ws;
    ws.workspace.present = true;
    ws.workspace.members = {"a"};
    auto first = member("a");
    first.package.standard = "c++26";
    first.toolchain.byPlatform["default"] = "gcc@16.1.0";
    first.buildConfig.dialectCxxflags = {"-freflection"};
    first.buildConfig.cxxflags = {"-DMEMBER=1"};
    first.buildConfig.ldflags = {"-lmember"};
    first.buildConfig.sources = {"src/**/*.cpp"};
    first.targets.push_back(mcpp::manifest::Target{.name = "a"});
    first.dependencies["fmt"] = mcpp::manifest::DependencySpec{.version = "11.0.0"};

    const auto v = mcpp::project::virtual_workspace_root(ws, first, "/ws");
    EXPECT_TRUE(v.package.virtualRoot);
    EXPECT_EQ(v.package.standard, "c++26");
    EXPECT_EQ(v.toolchain.byPlatform.at("default"), "gcc@16.1.0");
    EXPECT_EQ(v.buildConfig.dialectCxxflags, first.buildConfig.dialectCxxflags);
    EXPECT_TRUE(v.buildConfig.cxxflags.empty());
    EXPECT_TRUE(v.buildConfig.ldflags.empty());
    EXPECT_TRUE(v.buildConfig.sources.empty());
    EXPECT_TRUE(v.buildConfig.sourcesDeclared);
    EXPECT_TRUE(v.targets.empty());
    EXPECT_TRUE(v.dependencies.empty());
    // The root's values and the group's are one statement.
    EXPECT_EQ(mcpp::project::root_position_key(v), mcpp::project::root_position_key(first));
}

// A member's products are in `bin/<package name>/`, qualified when another
// member of the workspace has the same name; the workspace's own package
// keeps `bin/`.
TEST(WorkspacePlan, ProductDirectoriesAreNamedByPackage) {
    const std::vector<mcpp::project::WorkspaceMember> all = {
        listed("apps/cli", "", "cli"),
        listed("ns1/common", "ns1", "common"),
        listed("ns2/common", "ns2", "common"),
    };
    EXPECT_EQ(mcpp::project::product_directory_name(all, "apps/cli"), "cli");
    EXPECT_EQ(mcpp::project::product_directory_name(all, "ns1/common"), "ns1.common");
    EXPECT_EQ(mcpp::project::product_directory_name(all, "ns2/common"), "ns2.common");
    EXPECT_EQ(mcpp::project::product_directory_name(all, "."), "");
}

// A relative `[indices].path` a member declares was written in the member's
// directory, and one the workspace declares in the workspace's: both are
// anchored where they were written, so two members naming one tree share a
// configuration and a member's path never resolves against the plan's root.
TEST(WorkspacePlan, IndexPathsAreAnchoredWhereTheyWereWritten) {
    namespace fs = std::filesystem;
    const auto ws = fs::temp_directory_path()
        / std::format("mcpp-ws-index-{}", std::random_device{}());
    fs::create_directories(ws / "a");
    fs::create_directories(ws / "b");
    auto write = [](const fs::path& p, std::string_view text) {
        std::ofstream(p) << text;
    };
    write(ws / "mcpp.toml",
          "[workspace]\nmembers = [\"a\", \"b\"]\n\n[indices]\nx = { path = \"idx\" }\n");
    write(ws / "a" / "mcpp.toml", "[package]\nname = \"a\"\nversion = \"0.1.0\"\n");
    write(ws / "b" / "mcpp.toml",
          "[indices]\nx = { path = \"../idx\" }\n\n[package]\nname = \"b\"\nversion = \"0.1.0\"\n");
    auto root = mcpp::manifest::load(ws / "mcpp.toml");
    ASSERT_TRUE(root.has_value());
    auto a = mcpp::project::load_member_manifest(*root, ws, "a");
    auto b = mcpp::project::load_member_manifest(*root, ws, "b");
    ASSERT_TRUE(a.has_value()) << a.error();
    ASSERT_TRUE(b.has_value()) << b.error();
    EXPECT_EQ(a->indices.at("x").path, (ws / "idx").lexically_normal());
    EXPECT_EQ(b->indices.at("x").path, (ws / "idx").lexically_normal());
    EXPECT_EQ(mcpp::project::root_position_key(*a), mcpp::project::root_position_key(*b));
    std::error_code ec;
    fs::remove_all(ws, ec);
}

// THE WORKSPACE'S PROFILES ARE ROOT-POSITION VALUES (2026.10.5.2). A member
// inherits each `[profile.<name>]` it does not declare, and its own table of a
// name replaces the workspace's whole, so members without a profile of their
// own share one configuration with the workspace's profile in it.
TEST(WorkspacePlan, MembersInheritTheWorkspacesProfilesByName) {
    namespace fs = std::filesystem;
    const auto ws = fs::temp_directory_path()
        / std::format("mcpp-ws-profile-{}", std::random_device{}());
    fs::create_directories(ws / "a");
    fs::create_directories(ws / "b");
    auto write = [](const fs::path& p, std::string_view text) { std::ofstream(p) << text; };
    write(ws / "mcpp.toml",
          "[workspace]\nmembers = [\"a\", \"b\"]\n\n"
          "[profile.release]\nopt = 3\nldflags = [\"-Wl,-z,now\"]\n\n"
          "[profile.bench]\nopt = 2\n");
    write(ws / "a" / "mcpp.toml", "[package]\nname = \"a\"\nversion = \"0.1.0\"\n");
    write(ws / "b" / "mcpp.toml",
          "[package]\nname = \"b\"\nversion = \"0.1.0\"\n\n[profile.release]\nopt = 1\n");
    auto root = mcpp::manifest::load(ws / "mcpp.toml");
    ASSERT_TRUE(root.has_value());
    auto a = mcpp::project::load_member_manifest(*root, ws, "a");
    auto b = mcpp::project::load_member_manifest(*root, ws, "b");
    ASSERT_TRUE(a.has_value()) << a.error();
    ASSERT_TRUE(b.has_value()) << b.error();
    EXPECT_EQ(a->profiles.at("release").optLevel, "3");
    EXPECT_EQ(a->profiles.at("release").ldflags, (std::vector<std::string>{"-Wl,-z,now"}));
    EXPECT_EQ(b->profiles.at("release").optLevel, "1");
    EXPECT_TRUE(b->profiles.at("release").ldflags.empty());
    EXPECT_EQ(b->profiles.at("bench").optLevel, "2");
    std::error_code ec;
    fs::remove_all(ws, ec);
}

// THE ROOT PACKAGE OF A WORKSPACE IS A MEMBER (2026.10.5.2): it receives
// `[workspace.build]` once, before its own `[build]`, on every path that reads it.
TEST(WorkspacePlan, TheRootPackageInheritsTheWorkspaceBuildOnce) {
    namespace fs = std::filesystem;
    const auto ws = fs::temp_directory_path()
        / std::format("mcpp-ws-rooted-{}", std::random_device{}());
    fs::create_directories(ws / "lib");
    std::ofstream(ws / "mcpp.toml")
        << "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
           "[build]\ncxxflags = [\"-DOWN\"]\n\n"
           "[workspace]\nmembers = [\"lib\"]\n\n"
           "[workspace.build]\ncxxflags = [\"-DSHARED\"]\n";
    std::ofstream(ws / "lib" / "mcpp.toml") << "[package]\nname = \"lib\"\nversion = \"0.1.0\"\n";
    const std::vector<std::string> expected{"-DSHARED", "-DOWN"};
    auto effective = mcpp::project::load_effective_manifest(ws);
    ASSERT_TRUE(effective.has_value()) << effective.error();
    EXPECT_EQ(effective->manifest.buildConfig.cxxflags, expected);
    // The manifest a selection reads is the one already inherited; the member
    // "." loaded from it is not inherited a second time.
    auto dot = mcpp::project::load_member_manifest(effective->manifest, ws, ".");
    ASSERT_TRUE(dot.has_value()) << dot.error();
    EXPECT_EQ(dot->buildConfig.cxxflags, expected);
    auto raw = mcpp::manifest::load(ws / "mcpp.toml");
    ASSERT_TRUE(raw.has_value());
    auto fromRaw = mcpp::project::load_member_manifest(*raw, ws, ".");
    ASSERT_TRUE(fromRaw.has_value()) << fromRaw.error();
    EXPECT_EQ(fromRaw->buildConfig.cxxflags, expected);
    std::error_code ec;
    fs::remove_all(ws, ec);
}
