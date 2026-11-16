
import std;
import mcpplibs.mtest;
import mcpplibs.os.stdio;
import mcpp.manifest;
import mcpp.project;
import mcpp.build.prepare;

using namespace mcpplibs::mtest;

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
UTest every_table_row_is_parsed_and_inherited{"WorkspaceInheritance/EveryTableRowIsParsedAndInherited", [] {
    auto ws = mcpp::manifest::parse_string(workspace_declaring_every_key());
    if (auto&& failed = expect(fatal(ws.has_value())); !failed) failed << ws.error().format();
    expect(fatal(bool(ws->workspace.inherited.buildPresent)));

    auto member = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\n", "m/mcpp.toml", {.insideWorkspace = true});
    if (auto&& failed = expect(fatal(member.has_value())); !failed) failed << member.error().format();
    const std::filesystem::path wsRoot = "/ws";
    mcpp::project::inherit_workspace_build(*member, *ws, wsRoot);

    std::size_t scalars = 0;
    for (auto const& row : mcpp::manifest::kWorkspaceBuildKeys) {
        /* trace: std::string(row.key) */;
        auto const& parsed = ws->workspace.inherited.build;
        auto const& inherited = member->buildConfig;
        if (auto const* f = std::get_if<std::vector<std::string>
                mcpp::manifest::BuildConfig::*>(&row.field)) {
            const std::vector<std::string> want{std::format("ws_{}", row.key)};
            expect(eq(parsed.**f, want));
            expect(eq(inherited.**f, want));
        } else if (auto const* f = std::get_if<std::vector<std::filesystem::path>
                       mcpp::manifest::BuildConfig::*>(&row.field)) {
            expect(eq(as_strings(parsed.**f), std::vector<std::string>{std::format("ws_{}", row.key)}));
            // Relative include directories are anchored at the workspace root.
            expect(eq(as_strings(inherited.**f), std::vector<std::string>{
                          (wsRoot / std::format("ws_{}", row.key)).generic_string()}));
        } else if (auto const* f = std::get_if<std::string
                       mcpp::manifest::BuildConfig::*>(&row.field)) {
            ++scalars;
            auto it = kSampleScalar.find(std::string(row.key));
            expect(fatal(ne(it, kSampleScalar.end()))) << "no sample value for a new scalar row";
            expect(eq(parsed.**f, it->second));
            expect(eq(inherited.**f, it->second));
        }
    }
    expect(eq(scalars, kSampleScalar.size()));
}};

UTest unknown_key_error_lists_the_table{"WorkspaceInheritance/UnknownKeyErrorListsTheTable", [] {
    auto ws = mcpp::manifest::parse_string(
        "[workspace]\nmembers = [\"m\"]\n\n[workspace.build]\nno_such_key = []\n");
    expect(fatal(!ws.has_value()));
    const auto text = ws.error().format();
    for (auto const& row : mcpp::manifest::kWorkspaceBuildKeys)
        expect(ne(text.find(row.key), std::string::npos)) << row.key;
}};

// W7: `defines` is a set keyed by macro name.
UTest later_entry_replaces_earlier_in_place{"DefinesFold/LaterEntryReplacesEarlierInPlace", [] {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"X=1", "Y", "X=2"};
    mcpp::build::fold_build_defines_into_flags(bc);
    expect(eq(bc.cxxflags, (std::vector<std::string>{"-DX=2", "-DY"})));
    expect(eq(bc.cflags, (std::vector<std::string>{"-DX=2", "-DY"})));
    expect(bc.defines.empty());
}};

UTest bang_name_removes_the_name{"DefinesFold/BangNameRemovesTheName", [] {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"X=1", "!X", "Z"};
    mcpp::build::fold_build_defines_into_flags(bc);
    expect(eq(bc.cxxflags, (std::vector<std::string>{"-DZ"})));
}};

UTest entry_supersedes_a_dash_d_word_in_the_flag_lists{"DefinesFold/EntrySupersedesADashDWordInTheFlagLists", [] {
    mcpp::manifest::BuildConfig bc;
    bc.cxxflags = {"-DX=1", "-O2", "-DXY=1"};
    bc.cflags = {"-DX"};
    bc.defines = {"X=3"};
    mcpp::build::fold_build_defines_into_flags(bc);
    // `-DXY` is another name and stays; `-DX=1` and `-DX` are superseded.
    expect(eq(bc.cxxflags, (std::vector<std::string>{"-O2", "-DXY=1", "-DX=3"})));
    expect(eq(bc.cflags, (std::vector<std::string>{"-DX=3"})));
}};

// The layer-conditional pass folds a second time with only its own entries.
UTest second_pass_removes_a_word_the_first_pass_folded{"DefinesFold/SecondPassRemovesAWordTheFirstPassFolded", [] {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"X=1", "W"};
    mcpp::build::fold_build_defines_into_flags(bc);
    bc.defines = {"!X"};
    mcpp::build::fold_build_defines_into_flags(bc);
    expect(eq(bc.cxxflags, (std::vector<std::string>{"-DW"})));
}};

UTest value_with_space_stays_one_word_and_keyed{"DefinesFold/ValueWithSpaceStaysOneWordAndKeyed", [] {
    mcpp::manifest::BuildConfig bc;
    bc.defines = {"N=\"a b\""};
    mcpp::build::fold_build_defines_into_flags(bc);
    expect(fatal(eq(bc.cxxflags.size(), 1u)));
    bc.defines = {"N=2"};
    mcpp::build::fold_build_defines_into_flags(bc);
    expect(eq(bc.cxxflags, (std::vector<std::string>{"-DN=2"})));
}};

// W1: the snapshot refuses a manifest whose `defines` were not folded.
UTest unfolded_defines_are_an_internal_error{"SnapshotPostcondition/UnfoldedDefinesAreAnInternalError", [] {
    mcpp::manifest::Manifest m;
    m.package.name = "lib";
    expect(!(mcpp::build::unfolded_defines_error(m).has_value()));
    m.buildConfig.defines = {"WORKSPACE_DEFINE=1"};
    auto err = mcpp::build::unfolded_defines_error(m);
    expect(fatal(err.has_value()));
    expect(ne(err->find("internal error"), std::string::npos));
    expect(ne(err->find("'lib'"), std::string::npos));
    expect(ne(err->find("WORKSPACE_DEFINE=1"), std::string::npos));
    mcpp::build::fold_build_defines_into_flags(m.buildConfig);
    expect(!(mcpp::build::unfolded_defines_error(m).has_value()));
}};

// #713. A member inherits the workspace root's `[xlings.workspace]` entries,
// conditional rows included; a package the member declares itself keeps the
// member's address, because the nearer declaration wins (SPEC-004 §4.5).
UTest a_member_inherits_the_roots_entries_and_its_own_wins{"WorkspaceXlings/AMemberInheritsTheRootsEntriesAndItsOwnWins", [] {
    auto ws = mcpp::manifest::parse_string(
        "[workspace]\nmembers = [\"m\"]\n\n"
        "[xlings.workspace]\nninja = \"1.12.1\"\ncmake = \"3.30.0\"\n\n"
        "[target.'cfg(os = \"linux\")'.xlings.workspace]\npatchelf = \"0.18.0\"\n");
    if (auto&& failed = expect(fatal(ws.has_value())); !failed) failed << ws.error().format();
    auto member = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n"
        "[xlings.workspace]\ncmake = \"3.31.0\"\n",
        "m/mcpp.toml", {.insideWorkspace = true});
    if (auto&& failed = expect(fatal(member.has_value())); !failed) failed << member.error().format();

    mcpp::project::inherit_workspace_xlings(*member, *ws);

    auto has = [&](std::string_view needle) {
        return std::ranges::any_of(member->xlings.deps, [&](const std::string& a) {
            return a.find(needle) != std::string::npos;
        });
    };
    expect(bool(has("ninja@1.12.1")));
    expect(bool(has("cmake@3.31.0")));
    expect(!has("cmake@3.30.0"));
    // The conditional row travels as a row, decided by its selector at merge time.
    bool rowCarried = false;
    for (auto const& cc : member->conditionalConfigs)
        for (auto const& a : cc.xlings.deps)
            rowCarried = rowCarried || a.find("patchelf@0.18.0") != std::string::npos;
    expect(bool(rowCarried));
}};

// #714. An entry that says `workspace = true` and that no workspace resolved is
// refused by name, in every dependency table.
UTest an_unresolved_workspace_entry_is_named{"WorkspaceDependency/AnUnresolvedWorkspaceEntryIsNamed", [] {
    for (std::string_view table : {"dependencies", "dev-dependencies", "build-dependencies"}) {
        /* trace: std::string(table) */;
        auto m = mcpp::manifest::parse_string(std::format(
            "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n[{}]\nfmt = {{ workspace = true }}\n",
            table), "m/mcpp.toml", {.insideWorkspace = true});
        if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
        auto err = mcpp::project::unresolved_workspace_dependency_error(*m, "/p/m");
        expect(fatal(err.has_value()));
        if (auto&& failed = expect(ne(err->find(std::format("[{}] fmt", table)), std::string::npos)); !failed) failed << *err;
        if (auto&& failed = expect(ne(err->find("members"), std::string::npos)); !failed) failed << *err;
    }
    auto resolved = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n[dependencies]\nfmt = \"11.0.0\"\n");
    expect(fatal(resolved.has_value()));
    expect(!mcpp::project::unresolved_workspace_dependency_error(*resolved, "/p/m"));
}};

// #710. A host tool's toolchain is the one its own build would use: its host
// row, then `[toolchain]`, each after the root-position keys of the workspace
// that lists it. A member tool that declares nothing takes the workspace's.
UTest a_member_tool_reads_its_workspace_toolchain{"HostToolToolchain/AMemberToolReadsItsWorkspaceToolchain", [] {
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
    expect(fatal(tool.has_value()));
    // Compared through the value: with clang and the MSVC STL, constructing
    // std::optional<std::string> from a literal fails to instantiate in this
    // translation unit, which includes gtest's headers and imports std.
    auto tc15 = mcpp::build::host_tool_declared_toolchain(*tool, root / "tool", "linux");
    expect(fatal(tc15.has_value()));
    expect(eq(*tc15, "gcc@15.1.0"));

    // The tool's own declaration wins over the workspace's.
    write(root / "tool" / "mcpp.toml",
          "[package]\nname = \"tool\"\nversion = \"0.1.0\"\n\n"
          "[toolchain]\ndefault = \"gcc@16.1.0\"\n");
    tool = mcpp::manifest::load(root / "tool" / "mcpp.toml");
    expect(fatal(tool.has_value()));
    auto tc16 = mcpp::build::host_tool_declared_toolchain(*tool, root / "tool", "linux");
    expect(fatal(tc16.has_value()));
    expect(eq(*tc16, "gcc@16.1.0"));

    std::error_code ec;
    fs::remove_all(root, ec);
}};

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
        if (auto&& failed = expect(m.has_value()); !failed) failed << (m ? "" : m.error().format());
        return m ? std::move(*m) : mcpp::manifest::Manifest{};
    }
};

// A member's own package name selects it, its full path does, and so does
// its directory's last segment (docs/07 §5.3's historical spellings, kept as
// a fallback).
UTest package_name_path_and_basename_all_select_the_one_member{"ResolveMember/PackageNamePathAndBasenameAllSelectTheOneMember", [] {
    Fixture f("single");
    f.member("modules/base", "ws-base");
    auto ws = f.root_manifest("[workspace]\nmembers = [\"modules/base\"]\n");

    std::vector<std::string_view> filters{"ws-base", "base", "modules/base",
                                          "./modules/base", "modules/base/"};
#if defined(_WIN32)
    filters.push_back("modules\\base");
#endif
    for (std::string_view filter : filters) {
        /* trace: std::string(filter) */;
        auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
        auto m = mcpp::project::resolve_member_dir(ws, f.root, filter);
        auto warned = stderrCapture.text();
        if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error());
        expect(eq(*m, f.root / "modules/base"));
        expect(warned.empty()) << warned;
    }
}};

// Two members declare the same package name under different namespaces:
// the bare name is refused (naming both qualified names), and each
// qualified name selects its own member without any ambiguity.
UTest same_name_under_two_namespaces_refuses_the_bare_name_only{"ResolveMember/SameNameUnderTwoNamespacesRefusesTheBareNameOnly", [] {
    Fixture f("dup-ns");
    f.member("a", "ws-common", "ns1");
    f.member("b", "ws-common", "ns2");
    auto ws = f.root_manifest("[workspace]\nmembers = [\"a\", \"b\"]\n");

    auto bare = mcpp::project::resolve_member_dir(ws, f.root, "ws-common");
    expect(fatal(!bare.has_value()));
    if (auto&& failed = expect(ne(bare.error().find("ns1.ws-common"), std::string::npos)); !failed) failed << bare.error();
    if (auto&& failed = expect(ne(bare.error().find("ns2.ws-common"), std::string::npos)); !failed) failed << bare.error();

    auto qa = mcpp::project::resolve_member_dir(ws, f.root, "ns1.ws-common");
    if (auto&& failed = expect(fatal(qa.has_value())); !failed) failed << qa.error();
    expect(eq(*qa, f.root / "a"));

    auto qb = mcpp::project::resolve_member_dir(ws, f.root, "ns2.ws-common");
    if (auto&& failed = expect(fatal(qb.has_value())); !failed) failed << qb.error();
    expect(eq(*qb, f.root / "b"));
}};

// A value that is one member's package name and a different member's
// directory selects the package -- the option names a package -- and warns,
// naming the other member and its path.
UTest package_name_outranks_another_members_directory_and_warns{"ResolveMember/PackageNameOutranksAnotherMembersDirectoryAndWarns", [] {
    Fixture f("name-vs-dir");
    f.member("modules/base", "ws-base");
    f.member("ws-base", "other");
    auto ws = f.root_manifest(
        "[workspace]\nmembers = [\"modules/base\", \"ws-base\"]\n");

    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    auto m = mcpp::project::resolve_member_dir(ws, f.root, "ws-base");
    auto warned = stderrCapture.text();
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error());
    expect(eq(*m, f.root / "modules/base"));
    expect(ne(warned.find("modules/base"), std::string::npos)) << warned;
    expect(ne(warned.find("ws-base"), std::string::npos)) << warned;
}};

// Two members share a directory basename (no package-name collision): the
// first in `[workspace] members` is still selected (a script written against
// it keeps working), and a warning now names the other and its path.
UTest duplicate_basename_keeps_first_match_and_warns{"ResolveMember/DuplicateBasenameKeepsFirstMatchAndWarns", [] {
    Fixture f("dup-basename");
    f.member("apps/core", "coreapp");
    f.member("libs/core", "corelib");
    auto ws = f.root_manifest("[workspace]\nmembers = [\"apps/core\", \"libs/core\"]\n");

    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    auto m = mcpp::project::resolve_member_dir(ws, f.root, "core");
    auto warned = stderrCapture.text();
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error());
    expect(eq(*m, f.root / "apps/core"));
    expect(ne(warned.find("libs/core"), std::string::npos)) << warned;
}};

// A filter matching nothing lists every member with its package name and its
// path.
UTest not_found_lists_every_member_by_name_and_path{"ResolveMember/NotFoundListsEveryMemberByNameAndPath", [] {
    Fixture f("not-found");
    f.member("a", "widget", "acme");
    f.member("b", "gadget");
    auto ws = f.root_manifest("[workspace]\nmembers = [\"a\", \"b\"]\n");

    auto m = mcpp::project::resolve_member_dir(ws, f.root, "no-such-member");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().find("acme.widget"), std::string::npos)); !failed) failed << m.error();
    if (auto&& failed = expect(ne(m.error().find("'a'"), std::string::npos)); !failed) failed << m.error();
    if (auto&& failed = expect(ne(m.error().find("gadget"), std::string::npos)); !failed) failed << m.error();
    if (auto&& failed = expect(ne(m.error().find("'b'"), std::string::npos)); !failed) failed << m.error();
}};

// No filter on a rooted workspace acts on the root package; no filter on a
// virtual one is refused. Unaffected by #725, kept here as the resolver's
// baseline.
UTest no_filter_acts_on_rooted_root_and_refuses_virtual{"ResolveMember/NoFilterActsOnRootedRootAndRefusesVirtual", [] {
    Fixture f("no-filter");
    f.member("a", "a");
    auto rooted = f.root_manifest("[workspace]\nmembers = [\"a\"]\n");
    auto onRoot = mcpp::project::resolve_member_dir(rooted, f.root, "");
    if (auto&& failed = expect(fatal(onRoot.has_value())); !failed) failed << onRoot.error();
    expect(onRoot->empty());

    auto virt = mcpp::manifest::parse_string("[workspace]\nmembers = [\"a\"]\n");
    if (auto&& failed = expect(fatal(virt.has_value())); !failed) failed << virt.error().format();
    auto onVirtual = mcpp::project::resolve_member_dir(*virt, f.root, "");
    expect(fatal(!onVirtual.has_value()));
    expect(ne(onVirtual.error().find("--workspace"), std::string::npos));
}};

} // namespace resolve_member

// A `workspace = true` entry under a target selector, or behind a feature,
// receives the workspace's source as one in [dependencies] does: a git
// dependency inherited there resolved from the index before 2026.10.26.2.
UTest conditional_and_feature_tables_inherit_the_source{"WorkspaceInheritance/ConditionalAndFeatureTablesInheritTheSource", [] {
    auto ws = mcpp::manifest::parse_string(
        "[workspace]\nmembers = [\"m\"]\n\n"
        "[workspace.dependencies]\n"
        "lib = { git = \"https://example.invalid/lib.git\", branch = \"main\" }\n"
        "tool = { path = \"tools/tool\" }\n");
    if (auto&& failed = expect(fatal(ws.has_value())); !failed) failed << ws.error().format();
    auto m = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n"
        "[features]\nextra = []\n\n"
        "[feature-deps.extra]\ntool.workspace = true\n\n"
        "[target.'cfg(os = \"linux\")'.dependencies]\nlib.workspace = true\n");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    mcpp::project::merge_workspace_deps(*m, *ws, "/ws");
    expect(fatal(!(m->conditionalConfigs.empty())));
    bool found = false;
    for (auto const& c : m->conditionalConfigs) {
        auto it = c.dependencies.find("lib");
        if (it == c.dependencies.end()) continue;
        found = true;
        expect(!(it->second.inheritWorkspace));
        expect(eq(it->second.git, "https://example.invalid/lib.git"));
        expect(eq(it->second.gitRev, "main"));
    }
    expect(bool(found));
    expect(fatal(m->featureDeps.contains("extra")));
    auto const& tool = m->featureDeps.at("extra").at("tool");
    expect(!tool.inheritWorkspace);
    expect(!tool.path.empty());
    expect(!(mcpp::project::unresolved_workspace_dependency_error(*m, "/ws/m").has_value()));
}};

UTest an_unresolved_conditional_entry_is_named{"WorkspaceInheritance/AnUnresolvedConditionalEntryIsNamed", [] {
    auto m = mcpp::manifest::parse_string(
        "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n"
        "[target.'cfg(os = \"linux\")'.dependencies]\nlib.workspace = true\n");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto err = mcpp::project::unresolved_workspace_dependency_error(*m, "/m");
    expect(fatal(err.has_value()));
    if (auto&& failed = expect(ne(err->find("dependencies] lib"), std::string::npos)); !failed) failed << *err;
}};
