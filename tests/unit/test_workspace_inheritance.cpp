#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.project;
import mcpp.build.prepare;

// #690. The unit-level halves of the workspace-inheritance repair: the
// `[workspace.build]` key table (one statement of the inheritable subset), the
// keyed `defines` fold, and the snapshot's post-condition. The end-to-end
// halves (both graph positions, git-hosted members) are
// tests/e2e/321_workspace_inheritance.sh and
// tests/e2e/741_workspace_member_as_dependency.sh.

namespace {

// A value for every row of `kWorkspaceBuildKeys`. The table and this map must
// have the same keys: a row added without a value here fails
// `EveryTableRowIsParsedAndInherited` by name.
const std::map<std::string, std::string> kSampleScalar = {
    {"c_standard", "c17"},
    {"linkage", "static"},
    {"target", "x86_64-linux-musl"},
    {"cxx_runtime", "self-contained"},
    {"dependency_linkage", "static"},
    {"macos_deployment_target", "13.0"},
    {"ios_deployment_target", "15.0"},
};

std::string workspace_declaring_every_key() {
    std::string src =
        "[workspace]\nmembers = [\"m\"]\n\n[workspace.package]\nversion = \"0.1.0\"\n\n"
        "[workspace.build]\n";
    for (auto const& row : mcpp::manifest::kWorkspaceBuildKeys) {
        if (std::holds_alternative<std::string mcpp::manifest::BuildConfig::*>(row.field)) {
            auto it = kSampleScalar.find(std::string(row.key));
            src += std::format("{} = \"{}\"\n", row.key,
                               it == kSampleScalar.end() ? "missing-sample" : it->second);
        } else {
            src += std::format("{} = [\"ws_{}\"]\n", row.key, row.key);
        }
    }
    return src;
}

std::vector<std::string> as_strings(const std::vector<std::filesystem::path>& v) {
    std::vector<std::string> out;
    for (auto const& p : v) out.push_back(p.generic_string());
    return out;
}

} // namespace

// F3: a key the parser assigns and inherits must also be a key it accepts.
// `ios_deployment_target` was the counterexample.
TEST(WorkspaceInheritance, EveryTableRowIsParsedAndInherited) {
    auto ws = mcpp::manifest::parse_string(workspace_declaring_every_key());
    ASSERT_TRUE(ws.has_value()) << ws.error().format();
    ASSERT_TRUE(ws->workspace.inherited.buildPresent);

    auto member = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\n", "m/mcpp.toml", {.insideWorkspace = true});
    ASSERT_TRUE(member.has_value()) << member.error().format();
    const std::filesystem::path wsRoot = "/ws";
    mcpp::project::inherit_workspace_build(*member, *ws, wsRoot);

    std::size_t scalars = 0;
    for (auto const& row : mcpp::manifest::kWorkspaceBuildKeys) {
        SCOPED_TRACE(std::string(row.key));
        auto const& parsed = ws->workspace.inherited.build;
        auto const& inherited = member->buildConfig;
        if (auto const* f = std::get_if<std::vector<std::string>
                mcpp::manifest::BuildConfig::*>(&row.field)) {
            const std::vector<std::string> want{std::format("ws_{}", row.key)};
            EXPECT_EQ(parsed.**f, want);
            EXPECT_EQ(inherited.**f, want);
        } else if (auto const* f = std::get_if<std::vector<std::filesystem::path>
                       mcpp::manifest::BuildConfig::*>(&row.field)) {
            EXPECT_EQ(as_strings(parsed.**f),
                      std::vector<std::string>{std::format("ws_{}", row.key)});
            // Relative include directories are anchored at the workspace root.
            EXPECT_EQ(as_strings(inherited.**f),
                      std::vector<std::string>{
                          (wsRoot / std::format("ws_{}", row.key)).generic_string()});
        } else if (auto const* f = std::get_if<std::string
                       mcpp::manifest::BuildConfig::*>(&row.field)) {
            ++scalars;
            auto it = kSampleScalar.find(std::string(row.key));
            ASSERT_NE(it, kSampleScalar.end()) << "no sample value for a new scalar row";
            EXPECT_EQ(parsed.**f, it->second);
            EXPECT_EQ(inherited.**f, it->second);
        }
    }
    EXPECT_EQ(scalars, kSampleScalar.size());
}

TEST(WorkspaceInheritance, UnknownKeyErrorListsTheTable) {
    auto ws = mcpp::manifest::parse_string(
        "[workspace]\nmembers = [\"m\"]\n\n[workspace.build]\nno_such_key = []\n");
    ASSERT_FALSE(ws.has_value());
    const auto text = ws.error().format();
    for (auto const& row : mcpp::manifest::kWorkspaceBuildKeys)
        EXPECT_NE(text.find(row.key), std::string::npos) << row.key;
}

// W7: `defines` is a set keyed by macro name.
TEST(DefinesFold, LaterEntryReplacesEarlierInPlace) {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"X=1", "Y", "X=2"};
    mcpp::build::fold_build_defines_into_flags(bc);
    EXPECT_EQ(bc.cxxflags, (std::vector<std::string>{"-DX=2", "-DY"}));
    EXPECT_EQ(bc.cflags, (std::vector<std::string>{"-DX=2", "-DY"}));
    EXPECT_TRUE(bc.defines.empty());
}

TEST(DefinesFold, BangNameRemovesTheName) {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"X=1", "!X", "Z"};
    mcpp::build::fold_build_defines_into_flags(bc);
    EXPECT_EQ(bc.cxxflags, (std::vector<std::string>{"-DZ"}));
}

TEST(DefinesFold, EntrySupersedesADashDWordInTheFlagLists) {
    mcpp::manifest::BuildConfig bc;
    bc.cxxflags = {"-DX=1", "-O2", "-DXY=1"};
    bc.cflags = {"-DX"};
    bc.defines = {"X=3"};
    mcpp::build::fold_build_defines_into_flags(bc);
    // `-DXY` is another name and stays; `-DX=1` and `-DX` are superseded.
    EXPECT_EQ(bc.cxxflags, (std::vector<std::string>{"-O2", "-DXY=1", "-DX=3"}));
    EXPECT_EQ(bc.cflags, (std::vector<std::string>{"-DX=3"}));
}

// The layer-conditional pass folds a second time with only its own entries.
TEST(DefinesFold, SecondPassRemovesAWordTheFirstPassFolded) {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"X=1", "W"};
    mcpp::build::fold_build_defines_into_flags(bc);
    bc.defines = {"!X"};
    mcpp::build::fold_build_defines_into_flags(bc);
    EXPECT_EQ(bc.cxxflags, (std::vector<std::string>{"-DW"}));
}

TEST(DefinesFold, ValueWithSpaceStaysOneWordAndKeyed) {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"N=\"a b\""};
    mcpp::build::fold_build_defines_into_flags(bc);
    ASSERT_EQ(bc.cxxflags.size(), 1u);
    bc.defines = {"N=2"};
    mcpp::build::fold_build_defines_into_flags(bc);
    EXPECT_EQ(bc.cxxflags, (std::vector<std::string>{"-DN=2"}));
}

// W1: the snapshot refuses a manifest whose `defines` were not folded.
TEST(SnapshotPostcondition, UnfoldedDefinesAreAnInternalError) {
    mcpp::manifest::Manifest m;
    m.package.name = "lib";
    EXPECT_FALSE(mcpp::build::unfolded_defines_error(m).has_value());
    m.buildConfig.defines = {"WORKSPACE_DEFINE=1"};
    auto err = mcpp::build::unfolded_defines_error(m);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("internal error"), std::string::npos);
    EXPECT_NE(err->find("'lib'"), std::string::npos);
    EXPECT_NE(err->find("WORKSPACE_DEFINE=1"), std::string::npos);
    mcpp::build::fold_build_defines_into_flags(m.buildConfig);
    EXPECT_FALSE(mcpp::build::unfolded_defines_error(m).has_value());
}

// #713. A member inherits the workspace root's `[xlings.workspace]` entries,
// conditional rows included; a package the member declares itself keeps the
// member's address, because the nearer declaration wins (SPEC-004 §4.5).
TEST(WorkspaceXlings, AMemberInheritsTheRootsEntriesAndItsOwnWins) {
    auto ws = mcpp::manifest::parse_string(
        "[workspace]\nmembers = [\"m\"]\n\n"
        "[xlings.workspace]\nninja = \"1.12.1\"\ncmake = \"3.30.0\"\n\n"
        "[target.'cfg(os = \"linux\")'.xlings.workspace]\npatchelf = \"0.18.0\"\n");
    ASSERT_TRUE(ws.has_value()) << ws.error().format();
    auto member = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n"
        "[xlings.workspace]\ncmake = \"3.31.0\"\n",
        "m/mcpp.toml", {.insideWorkspace = true});
    ASSERT_TRUE(member.has_value()) << member.error().format();

    mcpp::project::inherit_workspace_xlings(*member, *ws);

    auto has = [&](std::string_view needle) {
        return std::ranges::any_of(member->xlings.deps, [&](const std::string& a) {
            return a.find(needle) != std::string::npos;
        });
    };
    EXPECT_TRUE(has("ninja@1.12.1"));
    EXPECT_TRUE(has("cmake@3.31.0"));
    EXPECT_FALSE(has("cmake@3.30.0"));
    // The conditional row travels as a row, decided by its selector at merge time.
    bool rowCarried = false;
    for (auto const& cc : member->conditionalConfigs)
        for (auto const& a : cc.xlings.deps)
            rowCarried = rowCarried || a.find("patchelf@0.18.0") != std::string::npos;
    EXPECT_TRUE(rowCarried);
}

// #714. An entry that says `workspace = true` and that no workspace resolved is
// refused by name, in every dependency table.
TEST(WorkspaceDependency, AnUnresolvedWorkspaceEntryIsNamed) {
    for (std::string_view table : {"dependencies", "dev-dependencies", "build-dependencies"}) {
        SCOPED_TRACE(std::string(table));
        auto m = mcpp::manifest::parse_string(std::format(
            "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n[{}]\nfmt = {{ workspace = true }}\n",
            table), "m/mcpp.toml", {.insideWorkspace = true});
        ASSERT_TRUE(m.has_value()) << m.error().format();
        auto err = mcpp::project::unresolved_workspace_dependency_error(*m, "/p/m");
        ASSERT_TRUE(err.has_value());
        EXPECT_NE(err->find(std::format("[{}] fmt", table)), std::string::npos) << *err;
        EXPECT_NE(err->find("members"), std::string::npos) << *err;
    }
    auto resolved = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n[dependencies]\nfmt = \"11.0.0\"\n");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_FALSE(mcpp::project::unresolved_workspace_dependency_error(*resolved, "/p/m"));
}

// #710. A host tool's toolchain is the one its own build would use: its host
// row, then `[toolchain]`, each after the root-position keys of the workspace
// that lists it. A member tool that declares nothing takes the workspace's.
TEST(HostToolToolchain, AMemberToolReadsItsWorkspaceToolchain) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("mcpp-710-{:x}", std::random_device{}());
    fs::create_directories(root / "tool");
    auto write = [](const fs::path& p, std::string_view text) {
        std::ofstream(p) << text;
    };
    write(root / "mcpp.toml",
          "[workspace]\nmembers = [\"tool\"]\n\n[toolchain]\ndefault = \"gcc@15.1.0\"\n");
    write(root / "tool" / "mcpp.toml",
          "[package]\nname = \"tool\"\nversion = \"0.1.0\"\n");
    auto tool = mcpp::manifest::load(root / "tool" / "mcpp.toml");
    ASSERT_TRUE(tool.has_value());
    // Compared through the value: with clang and the MSVC STL, constructing
    // std::optional<std::string> from a literal fails to instantiate in this
    // translation unit, which includes gtest's headers and imports std.
    auto tc15 = mcpp::build::host_tool_declared_toolchain(*tool, root / "tool", "linux");
    ASSERT_TRUE(tc15.has_value());
    EXPECT_EQ(*tc15, "gcc@15.1.0");

    // The tool's own declaration wins over the workspace's.
    write(root / "tool" / "mcpp.toml",
          "[package]\nname = \"tool\"\nversion = \"0.1.0\"\n\n"
          "[toolchain]\ndefault = \"gcc@16.1.0\"\n");
    tool = mcpp::manifest::load(root / "tool" / "mcpp.toml");
    ASSERT_TRUE(tool.has_value());
    auto tc16 = mcpp::build::host_tool_declared_toolchain(*tool, root / "tool", "linux");
    ASSERT_TRUE(tc16.has_value());
    EXPECT_EQ(*tc16, "gcc@16.1.0");

    std::error_code ec;
    fs::remove_all(root, ec);
}

// #725. `-p, --package <NAME>` resolves the package identity first, and the
// directory spellings docs/07 §5.3 has always documented second
// (mcpp::project::resolve_member_dir). One resolver serves every `-p`
// reading command; the e2e halves (the workspace-context repair itself, and
// `-p` on a real build) are tests/e2e/805_… and tests/e2e/806_….
namespace resolve_member {

namespace fs = std::filesystem;

struct Fixture {
    fs::path root;

    explicit Fixture(std::string_view tag) {
        root = fs::temp_directory_path()
             / std::format("mcpp-725-{}-{:x}", tag, std::random_device{}());
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Fixture(const Fixture&) = delete;

    void write(const fs::path& rel, std::string_view text) {
        auto p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }
    // `path` is written into `mcpp.toml` as the manifest's own [package]
    // fields; `dir` is the directory it lives in, relative to `root`.
    void member(std::string_view dir, std::string_view name,
                std::string_view ns = "") {
        std::string toml = "[package]\n";
        if (!ns.empty()) toml += std::format("namespace = \"{}\"\n", ns);
        toml += std::format("name = \"{}\"\n", name);
        write(fs::path(dir) / "mcpp.toml", toml);
    }
    mcpp::manifest::Manifest root_manifest(std::string_view membersToml) {
        write("mcpp.toml", std::format(
            "[package]\nname = \"root\"\nversion = \"0.1.0\"\n\n{}", membersToml));
        auto m = mcpp::manifest::load(root / "mcpp.toml");
        EXPECT_TRUE(m.has_value()) << (m ? "" : m.error().format());
        return m ? std::move(*m) : mcpp::manifest::Manifest{};
    }
};

// A member's own package name selects it, its full path does, and so does
// its directory's last segment (docs/07 §5.3's historical spellings, kept as
// a fallback).
TEST(ResolveMember, PackageNamePathAndBasenameAllSelectTheOneMember) {
    Fixture f("single");
    f.member("modules/base", "ws-base");
    auto ws = f.root_manifest("[workspace]\nmembers = [\"modules/base\"]\n");

    std::vector<std::string_view> filters{"ws-base", "base", "modules/base",
                                          "./modules/base", "modules/base/"};
#if defined(_WIN32)
    filters.push_back("modules\\base");
#endif
    for (std::string_view filter : filters) {
        SCOPED_TRACE(std::string(filter));
        testing::internal::CaptureStderr();
        auto m = mcpp::project::resolve_member_dir(ws, f.root, filter);
        auto warned = testing::internal::GetCapturedStderr();
        ASSERT_TRUE(m.has_value()) << (m ? "" : m.error());
        EXPECT_EQ(*m, f.root / "modules/base");
        EXPECT_TRUE(warned.empty()) << warned;
    }
}

// Two members declare the same package name under different namespaces:
// the bare name is refused (naming both qualified names), and each
// qualified name selects its own member without any ambiguity.
TEST(ResolveMember, SameNameUnderTwoNamespacesRefusesTheBareNameOnly) {
    Fixture f("dup-ns");
    f.member("a", "ws-common", "ns1");
    f.member("b", "ws-common", "ns2");
    auto ws = f.root_manifest("[workspace]\nmembers = [\"a\", \"b\"]\n");

    auto bare = mcpp::project::resolve_member_dir(ws, f.root, "ws-common");
    ASSERT_FALSE(bare.has_value());
    EXPECT_NE(bare.error().find("ns1.ws-common"), std::string::npos) << bare.error();
    EXPECT_NE(bare.error().find("ns2.ws-common"), std::string::npos) << bare.error();

    auto qa = mcpp::project::resolve_member_dir(ws, f.root, "ns1.ws-common");
    ASSERT_TRUE(qa.has_value()) << qa.error();
    EXPECT_EQ(*qa, f.root / "a");

    auto qb = mcpp::project::resolve_member_dir(ws, f.root, "ns2.ws-common");
    ASSERT_TRUE(qb.has_value()) << qb.error();
    EXPECT_EQ(*qb, f.root / "b");
}

// A value that is one member's package name and a different member's
// directory selects the package -- the option names a package -- and warns,
// naming the other member and its path.
TEST(ResolveMember, PackageNameOutranksAnotherMembersDirectoryAndWarns) {
    Fixture f("name-vs-dir");
    f.member("modules/base", "ws-base");
    f.member("ws-base", "other");
    auto ws = f.root_manifest(
        "[workspace]\nmembers = [\"modules/base\", \"ws-base\"]\n");

    testing::internal::CaptureStderr();
    auto m = mcpp::project::resolve_member_dir(ws, f.root, "ws-base");
    auto warned = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(m.has_value()) << (m ? "" : m.error());
    EXPECT_EQ(*m, f.root / "modules/base");
    EXPECT_NE(warned.find("modules/base"), std::string::npos) << warned;
    EXPECT_NE(warned.find("ws-base"), std::string::npos) << warned;
}

// Two members share a directory basename (no package-name collision): the
// first in `[workspace] members` is still selected (a script written against
// it keeps working), and a warning now names the other and its path.
TEST(ResolveMember, DuplicateBasenameKeepsFirstMatchAndWarns) {
    Fixture f("dup-basename");
    f.member("apps/core", "coreapp");
    f.member("libs/core", "corelib");
    auto ws = f.root_manifest("[workspace]\nmembers = [\"apps/core\", \"libs/core\"]\n");

    testing::internal::CaptureStderr();
    auto m = mcpp::project::resolve_member_dir(ws, f.root, "core");
    auto warned = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(m.has_value()) << (m ? "" : m.error());
    EXPECT_EQ(*m, f.root / "apps/core");
    EXPECT_NE(warned.find("libs/core"), std::string::npos) << warned;
}

// A filter matching nothing lists every member with its package name and its
// path.
TEST(ResolveMember, NotFoundListsEveryMemberByNameAndPath) {
    Fixture f("not-found");
    f.member("a", "widget", "acme");
    f.member("b", "gadget");
    auto ws = f.root_manifest("[workspace]\nmembers = [\"a\", \"b\"]\n");

    auto m = mcpp::project::resolve_member_dir(ws, f.root, "no-such-member");
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().find("acme.widget"), std::string::npos) << m.error();
    EXPECT_NE(m.error().find("'a'"), std::string::npos) << m.error();
    EXPECT_NE(m.error().find("gadget"), std::string::npos) << m.error();
    EXPECT_NE(m.error().find("'b'"), std::string::npos) << m.error();
}

// No filter on a rooted workspace acts on the root package; no filter on a
// virtual one is refused. Unaffected by #725, kept here as the resolver's
// baseline.
TEST(ResolveMember, NoFilterActsOnRootedRootAndRefusesVirtual) {
    Fixture f("no-filter");
    f.member("a", "a");
    auto rooted = f.root_manifest("[workspace]\nmembers = [\"a\"]\n");
    auto onRoot = mcpp::project::resolve_member_dir(rooted, f.root, "");
    ASSERT_TRUE(onRoot.has_value()) << onRoot.error();
    EXPECT_TRUE(onRoot->empty());

    auto virt = mcpp::manifest::parse_string("[workspace]\nmembers = [\"a\"]\n");
    ASSERT_TRUE(virt.has_value()) << virt.error().format();
    auto onVirtual = mcpp::project::resolve_member_dir(*virt, f.root, "");
    ASSERT_FALSE(onVirtual.has_value());
    EXPECT_NE(onVirtual.error().find("--workspace"), std::string::npos);
}

} // namespace resolve_member

// A `workspace = true` entry under a target selector, or behind a feature,
// receives the workspace's source as one in [dependencies] does: a git
// dependency inherited there resolved from the index before 2026.10.26.2.
TEST(WorkspaceInheritance, ConditionalAndFeatureTablesInheritTheSource) {
    auto ws = mcpp::manifest::parse_string(
        "[workspace]\nmembers = [\"m\"]\n\n"
        "[workspace.dependencies]\n"
        "lib = { git = \"https://example.invalid/lib.git\", branch = \"main\" }\n"
        "tool = { path = \"tools/tool\" }\n");
    ASSERT_TRUE(ws.has_value()) << ws.error().format();
    auto m = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n"
        "[features]\nextra = []\n\n"
        "[feature-deps.extra]\ntool.workspace = true\n\n"
        "[target.'cfg(os = \"linux\")'.dependencies]\nlib.workspace = true\n");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    mcpp::project::merge_workspace_deps(*m, *ws, "/ws");
    ASSERT_FALSE(m->conditionalConfigs.empty());
    bool found = false;
    for (auto const& c : m->conditionalConfigs) {
        auto it = c.dependencies.find("lib");
        if (it == c.dependencies.end()) continue;
        found = true;
        EXPECT_FALSE(it->second.inheritWorkspace);
        EXPECT_EQ(it->second.git, "https://example.invalid/lib.git");
        EXPECT_EQ(it->second.gitRev, "main");
    }
    EXPECT_TRUE(found);
    ASSERT_TRUE(m->featureDeps.contains("extra"));
    auto const& tool = m->featureDeps.at("extra").at("tool");
    EXPECT_FALSE(tool.inheritWorkspace);
    EXPECT_FALSE(tool.path.empty());
    EXPECT_FALSE(mcpp::project::unresolved_workspace_dependency_error(*m, "/ws/m").has_value());
}

TEST(WorkspaceInheritance, AnUnresolvedConditionalEntryIsNamed) {
    auto m = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n"
        "[target.'cfg(os = \"linux\")'.dependencies]\nlib.workspace = true\n");
    ASSERT_TRUE(m.has_value()) << m.error().format();
    auto err = mcpp::project::unresolved_workspace_dependency_error(*m, "/m");
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("dependencies] lib"), std::string::npos) << *err;
}
