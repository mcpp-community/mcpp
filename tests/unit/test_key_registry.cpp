#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.project;

// SPEC-004 §9.10: the key registry, the workspace layer and the merge rules
// (W1-W3, W7). The end-to-end halves are tests/e2e/893-895 and 900.

namespace mm = mcpp::manifest;

namespace {

mm::Manifest parse(std::string_view text) {
    auto m = mm::parse_string(text);
    if (!m) ADD_FAILURE() << m.error().format();
    return m ? *m : mm::Manifest{};
}

std::string parse_error(std::string_view text) {
    auto m = mm::parse_string(text);
    return m ? std::string{} : m.error().message;
}

}  // namespace

// The inheritable subset of [build] is stated by two tables: the registry's
// `shared` column and `kWorkspaceBuildKeys`, which also carries the field each
// key fills. They must name the same keys; `linkage` is the one key
// [workspace.build] reads that [build] does not (the target row and the
// profile set it there).
TEST(KeyRegistry, TheSharedBuildKeysAreTheWorkspaceBuildKeys) {
    std::set<std::string> shared, table;
    for (auto const& row : mm::kKeyRegistry)
        if (row.table == "build" && row.shared) shared.emplace(row.key);
    for (auto const& row : mm::kWorkspaceBuildKeys)
        if (row.key != "linkage") table.emplace(row.key);
    EXPECT_EQ(shared, table);
}

// Every configuration key of [build] is part of the value members are grouped
// by: two members that differ in one are not one plan.
TEST(KeyRegistry, EveryConfigurationKeyEntersTheGroupingKey) {
    const auto key = mcpp::project::root_position_key(mm::Manifest{});
    const std::map<std::string_view, std::string_view> spelled = {
        {"profile", "default_profile"}, {"default-profile", "default_profile"},
        {"platform-dependencies", "platform_dependencies"},
    };
    for (auto const& row : mm::kKeyRegistry) {
        if (row.table != "build" || row.scope != mm::KeyScope::Configuration) continue;
        auto it = spelled.find(row.key);
        const std::string name(it == spelled.end() ? row.key : it->second);
        EXPECT_NE(key.find(name + "="), std::string::npos) << row.key;
    }
}

// The parser accepts every [build] key of the registry without a warning.
TEST(KeyRegistry, TheParserReadsTheRegistrysBuildKeys) {
    auto m = parse(R"(
[package]
name = "x"
version = "0.1.0"
[build]
sources = ["src/*.cpp"]
cxxflags = ["-O2"]
dialect_cxxflags = ["-DX=1"]
cache = "local"
)");
    EXPECT_TRUE(m.schemaWarnings.empty());
    auto w = parse(R"(
[package]
name = "x"
version = "0.1.0"
[build]
cxxflagz = ["-O2"]
)");
    ASSERT_EQ(w.schemaWarnings.size(), 1u);
    EXPECT_NE(w.schemaWarnings.front().find("cxxflagz"), std::string::npos);
}

// W2: [workspace.X] is read by the reader of X.
TEST(WorkspaceLayer, MirrorsAreReadByTheReaderOfTheTableTheyMirror) {
    auto ws = parse(R"(
[workspace]
members = ["a"]
[workspace.toolchain]
default = "llvm@23.1.3"
[workspace.profile.release]
opt = 3
[workspace.target.x86_64-linux-musl]
linkage = "static"
[workspace.target.'cfg(os = "linux")'.build]
dialect_cxxflags = ["-DW=1"]
cxxflags = ["-DSHARED"]
[workspace.xlings.workspace]
cmake = "4.0.2"
)");
    ASSERT_TRUE(ws.layer);
    auto const& layer = *ws.layer;
    EXPECT_EQ(layer.toolchain.for_platform("linux").value_or(""), "llvm@23.1.3");
    ASSERT_TRUE(layer.profiles.contains("release"));
    EXPECT_EQ(layer.profiles.at("release").optLevel, "3");
    EXPECT_TRUE(layer.profiles.at("release").optDeclared);
    EXPECT_FALSE(layer.profiles.at("release").debugDeclared);
    EXPECT_EQ(layer.targetOverrides.at("x86_64-linux-musl").linkage, "static");
    ASSERT_FALSE(layer.conditionalConfigs.empty());
    EXPECT_EQ(ws.workspace.layerTables,
              (std::vector<std::string>{"toolchain", "profile", "target", "xlings"}));
    // The root itself states none of them for its own package.
    EXPECT_TRUE(ws.toolchain.byPlatform.empty());
    EXPECT_TRUE(ws.targetOverrides.empty());
}

TEST(WorkspaceLayer, RefusesWhatDescribesOnePackageAndWhatItDoesNotKnow) {
    EXPECT_NE(parse_error(R"(
[workspace]
members = ["a"]
[workspace.tolchain]
default = "gcc@16.1.0"
)").find("[workspace] has no key or table 'tolchain'"), std::string::npos);
    EXPECT_NE(parse_error(R"(
[workspace]
members = ["a"]
[workspace.target.x86_64-linux-gnu.dependencies]
fmt = "11.0.0"
)").find("describes one package"), std::string::npos);
    EXPECT_NE(parse_error(R"(
[workspace]
members = ["a"]
[workspace.target.x86_64-linux-gnu.build]
sources = ["x.cpp"]
)").find("has no key 'sources'"), std::string::npos);
    EXPECT_NE(parse_error(R"(
[workspace]
members = ["a"]
[workspace.target.x86_64-linux-gnu.build]
allow_host_libs = true
)").find("allow_host_libs is not inheritable"), std::string::npos);
    // On a root without [package], one table cannot speak to members twice.
    EXPECT_NE(parse_error(R"(
[workspace]
members = ["a"]
[toolchain]
default = "gcc@16.1.0"
[workspace.toolchain]
default = "llvm@23.1.3"
)").find("write it once, as [workspace.toolchain]"), std::string::npos);
    // With [package], the root-position table is the root package's own.
    EXPECT_TRUE(parse_error(R"(
[package]
name = "root"
version = "0.1.0"
[workspace]
members = ["a"]
[toolchain]
default = "gcc@16.1.0"
[workspace.toolchain]
default = "llvm@23.1.3"
)").empty());
}

// W7: a package table on a root without [package] acts on nothing.
TEST(WorkspaceLayer, PackageTablesOnAVirtualRootAreRecorded) {
    auto ws = parse(R"(
[workspace]
members = ["a"]
[build]
cxxflags = ["-O2"]
[target.'cfg(os = "linux")'.build]
dialect_cxxflags = ["-DX=1"]
)");
    ASSERT_EQ(ws.virtualRootPackageTables.size(), 2u);
    EXPECT_NE(ws.virtualRootPackageTables[0].find("[build]"), std::string::npos);
    EXPECT_NE(ws.virtualRootPackageTables[1].find("[workspace.target."), std::string::npos);
    auto rooted = parse(R"(
[package]
name = "root"
version = "0.1.0"
[workspace]
members = ["a"]
[build]
cxxflags = ["-O2"]
)");
    EXPECT_TRUE(rooted.virtualRootPackageTables.empty());
}

// W3: scalars the member declared are the member's, named tables merge key by
// key, lists put the workspace first, conditional rows come first.
TEST(WorkspaceLayer, TheMergeRuleIsTheKeys) {
    auto ws = parse(R"(
[workspace]
members = ["a"]
[workspace.toolchain]
linux = "llvm@23.1.3"
macos = "llvm@23.1.3"
[workspace.profile.release]
opt = 3
lto = true
cxxflags = ["-DWS"]
[workspace.target.x86_64-linux-musl]
linkage = "static"
cxx_runtime = "self-contained"
[workspace.target.'cfg(os = "linux")'.build]
dialect_cxxflags = ["-DW=1"]
)");
    auto member = parse(R"(
[package]
name = "a"
version = "0.1.0"
[toolchain]
linux = "gcc@16.1.0"
[profile.release]
opt = 2
cxxflags = ["-DMEMBER"]
[target.x86_64-linux-musl]
linkage = "dynamic"
[target.'cfg(os = "linux")'.build]
dialect_cxxflags = ["-DM=1"]
)");
    mcpp::project::inherit_workspace_root_position(member, ws, "/ws");
    mcpp::project::inherit_workspace_layer_rows(member, ws, "/ws");
    EXPECT_EQ(member.toolchain.for_platform("linux").value_or(""), "gcc@16.1.0");
    EXPECT_EQ(member.toolchain.for_platform("macos").value_or(""), "llvm@23.1.3");
    auto const& r = member.profiles.at("release");
    EXPECT_EQ(r.optLevel, "2");
    EXPECT_TRUE(r.lto);
    EXPECT_EQ(r.cxxflags, (std::vector<std::string>{"-DWS", "-DMEMBER"}));
    auto const& t = member.targetOverrides.at("x86_64-linux-musl");
    EXPECT_EQ(t.linkage, "dynamic");
    EXPECT_EQ(t.cxxRuntime, "self-contained");
    ASSERT_GE(member.conditionalConfigs.size(), 2u);
    EXPECT_EQ(member.conditionalConfigs.front().dialectCxxflags,
              (std::vector<std::string>{"-DW=1"}));

    // A member's own `default` speaks for every platform.
    auto own = parse(R"(
[package]
name = "b"
version = "0.1.0"
[toolchain]
default = "gcc@16.1.0"
)");
    mcpp::project::inherit_workspace_root_position(own, ws, "/ws");
    EXPECT_EQ(own.toolchain.for_platform("macos").value_or(""), "gcc@16.1.0");
}

// W4: grouping counts a conditional configuration row as it evaluates.
TEST(WorkspaceLayer, GroupingReadsEvaluatedRows) {
    auto a = parse(R"(
[package]
name = "a"
version = "0.1.0"
[target.'cfg(os = "windows")'.build]
dialect_cxxflags = ["-DWIN=1"]
)");
    auto b = parse(R"(
[package]
name = "b"
version = "0.1.0"
)");
    auto linux = [](std::string_view p) { return p.find("linux") != std::string_view::npos; };
    auto windows = [](std::string_view p) { return p.find("windows") != std::string_view::npos; };
    EXPECT_EQ(mcpp::project::root_position_key(a, linux), mcpp::project::root_position_key(b, linux));
    EXPECT_NE(mcpp::project::root_position_key(a, windows), mcpp::project::root_position_key(b, windows));
}
