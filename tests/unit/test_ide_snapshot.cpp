#include <gtest/gtest.h>

import std;
import mcpp.ide.model;
import mcpp.ide.inspect;
import mcpp.manifest;

namespace {

struct TempProject {
    std::filesystem::path root;

    TempProject() {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp_ide_{}", std::chrono::steady_clock::now()
                                                .time_since_epoch().count());
        std::filesystem::create_directories(root);
    }

    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    void write(std::string_view relative, std::string_view content) const {
        auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << content;
    }
};

const mcpp::ide::Diagnostic* diagnostic(const mcpp::ide::WorkspaceInspection& snapshot,
                                        std::string_view code) {
    for (const auto& item : snapshot.diagnostics)
        if (item.code == code) return &item;
    return nullptr;
}

using Inventory = std::map<std::filesystem::path,
                           std::tuple<std::uintmax_t, std::filesystem::file_time_type>>;

Inventory inventory(const std::filesystem::path& root) {
    Inventory result;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto relative = std::filesystem::relative(it->path(), root, ec);
        if (ec) { ec.clear(); continue; }
        const auto size = std::filesystem::is_regular_file(it->path(), ec)
                        ? std::filesystem::file_size(it->path(), ec) : 0;
        ec.clear();
        const auto stamp = std::filesystem::last_write_time(it->path(), ec);
        if (ec) { ec.clear(); continue; }
        result.emplace(relative, std::tuple{size, stamp});
    }
    return result;
}

std::string package_manifest(std::string_view name = "app",
                             std::string_view version = "1.2.3") {
    return std::format("[package]\nname=\"{}\"\nversion=\"{}\"\n", name, version);
}

} // namespace

TEST(IdeSnapshotModel, WireNamesAreStable) {
    using namespace mcpp::ide;
    EXPECT_EQ(wire_name(SnapshotState::Partial), "partial");
    EXPECT_EQ(wire_name(SnapshotState::Stale), "stale");
    EXPECT_EQ(wire_name(SnapshotState::Unavailable), "unavailable");
    EXPECT_EQ(wire_name(SnapshotState::Ready), "ready");
    EXPECT_EQ(wire_name(ArtifactState::Missing), "missing");
    EXPECT_EQ(wire_name(ArtifactState::Stale), "stale");
    EXPECT_EQ(wire_name(Severity::Warning), "warning");
    EXPECT_EQ(wire_name(Severity::Error), "error");
}

TEST(IdeSnapshotInspect, SinglePackageIsPartialWithoutCdb) {
    TempProject p;
    p.write("mcpp.toml", package_manifest());
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root});
    ASSERT_EQ(snapshot.state, mcpp::ide::SnapshotState::Partial);
    ASSERT_EQ(snapshot.members.size(), 1u);
    EXPECT_EQ(snapshot.members[0].name, "app");
    ASSERT_EQ(snapshot.selectedMembers, std::vector<std::string>{"app"});
    ASSERT_EQ(snapshot.compileCommands.size(), 1u);
    EXPECT_EQ(snapshot.compileCommands[0].state, mcpp::ide::ArtifactState::Missing);
    ASSERT_NE(diagnostic(snapshot, "MCPP_IDE_ARTIFACTS_MISSING"), nullptr);
}

TEST(IdeSnapshotInspect, ExistingCdbIsStaleNeverReady) {
    TempProject p;
    p.write("mcpp.toml", package_manifest());
    p.write("compile_commands.json", "[]\n");
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root});
    EXPECT_EQ(snapshot.state, mcpp::ide::SnapshotState::Stale);
    ASSERT_EQ(snapshot.compileCommands.size(), 1u);
    EXPECT_EQ(snapshot.compileCommands[0].state, mcpp::ide::ArtifactState::Stale);
    EXPECT_NE(diagnostic(snapshot, "MCPP_IDE_ARTIFACTS_UNVERIFIED"), nullptr);
}

TEST(IdeSnapshotInspect, NoManifestIsUnavailable) {
    TempProject p;
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root});
    EXPECT_EQ(snapshot.state, mcpp::ide::SnapshotState::Unavailable);
    EXPECT_NE(diagnostic(snapshot, "MCPP_IDE_MANIFEST_NOT_FOUND"), nullptr);
}

TEST(IdeSnapshotInspect, InvalidManifestPreservesLocation) {
    TempProject p;
    p.write("mcpp.toml", "[package\nname=\"app\"\n");
    auto expected = mcpp::manifest::load(p.root / "mcpp.toml");
    ASSERT_FALSE(expected.has_value());
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root});
    EXPECT_EQ(snapshot.state, mcpp::ide::SnapshotState::Unavailable);
    auto* item = diagnostic(snapshot, "MCPP_IDE_MANIFEST_INVALID");
    ASSERT_NE(item, nullptr);
    ASSERT_TRUE(item->range.has_value());
    EXPECT_EQ(item->path, expected.error().file);
    EXPECT_EQ(item->range->start.line, expected.error().line);
    EXPECT_EQ(item->range->start.column, expected.error().column);
}

TEST(IdeSnapshotInspect, VirtualWorkspaceSelectsAllMembers) {
    TempProject p;
    p.write("mcpp.toml", "[workspace]\nmembers=[\"one\",\"two\"]\n");
    p.write("one/mcpp.toml", package_manifest("one", "1.0.0"));
    p.write("two/mcpp.toml", package_manifest("two", "2.0.0"));
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root});
    EXPECT_EQ(snapshot.state, mcpp::ide::SnapshotState::Partial);
    ASSERT_EQ(snapshot.members.size(), 2u);
    EXPECT_EQ(snapshot.members[0].workspacePath, "one");
    EXPECT_EQ(snapshot.members[1].workspacePath, "two");
    EXPECT_EQ(snapshot.selectedMembers, (std::vector<std::string>{"one", "two"}));
}

TEST(IdeSnapshotInspect, RootedWorkspaceSelectsRoot) {
    TempProject p;
    p.write("mcpp.toml", package_manifest("root") + "[workspace]\nmembers=[\"member\"]\n");
    p.write("member/mcpp.toml", package_manifest("member"));
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root});
    ASSERT_EQ(snapshot.members.size(), 2u);
    EXPECT_EQ(snapshot.members[0].workspacePath, ".");
    EXPECT_EQ(snapshot.selectedMembers, std::vector<std::string>{"root"});
}

TEST(IdeSnapshotInspect, MemberDirectorySelectsCurrentMember) {
    TempProject p;
    p.write("mcpp.toml", "[workspace]\nmembers=[\"member\"]\n");
    p.write("member/mcpp.toml", package_manifest("member"));
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root / "member"});
    EXPECT_EQ(snapshot.selectedMembers, std::vector<std::string>{"member"});
}

TEST(IdeSnapshotInspect, PackageSelectorMatchesBasenameAndPath) {
    TempProject p;
    p.write("mcpp.toml", "[workspace]\nmembers=[\"libs/one\",\"apps/two\"]\n");
    p.write("libs/one/mcpp.toml", package_manifest("first"));
    p.write("apps/two/mcpp.toml", package_manifest("second"));
    auto by_name = mcpp::ide::inspect_workspace({.start = p.root,
        .selectors = {.package = "two"}});
    EXPECT_EQ(by_name.selectedMembers, std::vector<std::string>{"second"});
    auto by_path = mcpp::ide::inspect_workspace({.start = p.root,
        .selectors = {.package = "libs/one"}});
    EXPECT_EQ(by_path.selectedMembers, std::vector<std::string>{"first"});
}

TEST(IdeSnapshotInspect, PackageSelectorPathWinsOverNameCollision) {
    TempProject p;
    p.write("mcpp.toml", package_manifest("app") + "[workspace]\nmembers=[\"app\"]\n");
    p.write("app/mcpp.toml", package_manifest("member"));
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root,
        .selectors = {.package = "app"}});
    EXPECT_EQ(snapshot.selectedMembers, std::vector<std::string>{"member"});
}

TEST(IdeSnapshotInspect, PackageSelectorDoesNotUseDottedTailAlias) {
    TempProject p;
    p.write("mcpp.toml", "[workspace]\nmembers=[\"libs/special\"]\n");
    p.write("libs/special/mcpp.toml", package_manifest("acme.foo"));
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root,
        .selectors = {.package = "foo"}});
    EXPECT_EQ(snapshot.state, mcpp::ide::SnapshotState::Unavailable);
    EXPECT_NE(diagnostic(snapshot, "MCPP_IDE_WORKSPACE_MEMBER_NOT_FOUND"), nullptr);
}

TEST(IdeSnapshotInspect, UnknownPackageUnavailable) {
    TempProject p;
    p.write("mcpp.toml", "[workspace]\nmembers=[\"one\"]\n");
    p.write("one/mcpp.toml", package_manifest("one"));
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root,
        .selectors = {.package = "missing"}});
    EXPECT_EQ(snapshot.state, mcpp::ide::SnapshotState::Unavailable);
    EXPECT_NE(diagnostic(snapshot, "MCPP_IDE_WORKSPACE_MEMBER_NOT_FOUND"), nullptr);
}

TEST(IdeSnapshotInspect, UnselectedMemberErrorIsDiagnosticOnly) {
    TempProject p;
    p.write("mcpp.toml", package_manifest("root") + "[workspace]\nmembers=[\"member\"]\n");
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root});
    EXPECT_EQ(snapshot.selectedMembers, std::vector<std::string>{"root"});
    EXPECT_NE(diagnostic(snapshot, "MCPP_IDE_MEMBER_MANIFEST_MISSING"), nullptr);
    EXPECT_NE(snapshot.state, mcpp::ide::SnapshotState::Unavailable);
}

TEST(IdeSnapshotInspect, SelectedMemberErrorIsUnavailable) {
    TempProject p;
    p.write("mcpp.toml", "[workspace]\nmembers=[\"member\"]\n");
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root,
        .selectors = {.package = "member"}});
    EXPECT_EQ(snapshot.state, mcpp::ide::SnapshotState::Unavailable);
    EXPECT_NE(diagnostic(snapshot, "MCPP_IDE_MEMBER_MANIFEST_MISSING"), nullptr);
}

TEST(IdeSnapshotInspect, TargetKindsAndMain) {
    TempProject p;
    p.write("mcpp.toml", package_manifest() +
        "[targets.library]\nkind=\"lib\"\n"
        "[targets.app]\nkind=\"bin\"\nmain=\"src/main.cpp\"\n"
        "[targets.shared]\nkind=\"shared\"\n"
        "[targets.test]\nkind=\"bin\"\nmain=\"tests/main.cpp\"\n");
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root});
    ASSERT_EQ(snapshot.members.size(), 1u);
    ASSERT_EQ(snapshot.members[0].targets.size(), 4u);
    EXPECT_EQ(snapshot.members[0].targets[0].name, "app");
    EXPECT_EQ(snapshot.members[0].targets[0].kind, "binary");
    EXPECT_EQ(snapshot.members[0].targets[0].main, std::optional<std::string>("src/main.cpp"));
    EXPECT_EQ(snapshot.members[0].targets[1].name, "library");
    EXPECT_EQ(snapshot.members[0].targets[1].kind, "library");
    EXPECT_EQ(snapshot.members[0].targets[2].kind, "shared-library");
    EXPECT_EQ(snapshot.members[0].targets[3].kind, "binary");
    EXPECT_EQ(snapshot.members[0].targets[3].main, std::optional<std::string>("tests/main.cpp"));
}

TEST(IdeSnapshotInspect, ReadOnlyInventory) {
    TempProject p;
    p.write("mcpp.toml", package_manifest());
    p.write("src/module.cppm", "export module app;\n");
    const auto before = inventory(p.root);
    auto snapshot = mcpp::ide::inspect_workspace({.start = p.root});
    const auto after = inventory(p.root);
    EXPECT_EQ(snapshot.state, mcpp::ide::SnapshotState::Partial);
    EXPECT_EQ(before, after);
}
