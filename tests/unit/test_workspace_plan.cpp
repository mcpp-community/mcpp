
import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.project;

using namespace mcpplibs::mtest;

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
UTest package_attributes_do_not_separate_members{"WorkspacePlan/PackageAttributesDoNotSeparateMembers", [] {
    auto a = member("a");
    auto b = member("b");
    b.buildConfig.cxxflags = {"-DONLY_B=1"};
    b.buildConfig.ldflags = {"-lm"};
    b.buildConfig.sources = {"src/**/*.cpp"};
    b.buildConfig.cStandard = "c17";
    b.dependencies["fmt"] = mcpp::manifest::DependencySpec{.version = "11.0.0"};
    expect(eq(mcpp::project::root_position_key(a), mcpp::project::root_position_key(b)));
}};

// What every node of one graph shares does separate them: the toolchain, the
// standard, the dialect flags, the target and the profile.
UTest root_position_values_separate_members{"WorkspacePlan/RootPositionValuesSeparateMembers", [] {
    const auto ref = mcpp::project::root_position_key(member("a"));
    { auto m = member("a"); m.package.standard = "c++26";
      expect(ne(mcpp::project::root_position_key(m), ref)); }
    { auto m = member("a"); m.toolchain.byPlatform["default"] = "gcc@16.1.0";
      expect(ne(mcpp::project::root_position_key(m), ref)); }
    { auto m = member("a"); m.buildConfig.dialectCxxflags = {"-freflection"};
      expect(ne(mcpp::project::root_position_key(m), ref)); }
    { auto m = member("a"); m.buildConfig.target = "x86_64-linux-musl";
      expect(ne(mcpp::project::root_position_key(m), ref)); }
    { auto m = member("a"); m.buildConfig.defaultProfile = "release";
      expect(ne(mcpp::project::root_position_key(m), ref)); }
    { auto m = member("a"); m.buildConfig.cxxRuntime = "static";
      expect(ne(mcpp::project::root_position_key(m), ref)); }
    // `linkage` chooses the C runtime every object of the plan is compiled
    // against, so it is the plan's and not the member's.
    { auto m = member("a"); m.buildConfig.linkage = "static";
      expect(ne(mcpp::project::root_position_key(m), ref)); }
}};

// The virtual root carries the plan's values and nothing a package owns.
UTest the_virtual_root_holds_no_package_content{"WorkspacePlan/TheVirtualRootHoldsNoPackageContent", [] {
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
    expect(bool(v.package.virtualRoot));
    expect(eq(v.package.standard, "c++26"));
    expect(eq(v.toolchain.byPlatform.at("default"), "gcc@16.1.0"));
    expect(eq(v.buildConfig.dialectCxxflags, first.buildConfig.dialectCxxflags));
    expect(v.buildConfig.cxxflags.empty());
    expect(v.buildConfig.ldflags.empty());
    expect(v.buildConfig.sources.empty());
    expect(bool(v.buildConfig.sourcesDeclared));
    expect(v.targets.empty());
    expect(v.dependencies.empty());
    // The root's values and the group's are one statement.
    expect(eq(mcpp::project::root_position_key(v), mcpp::project::root_position_key(first)));
}};

// A member's products are in `bin/<package name>/`, qualified when another
// member of the workspace has the same name; the workspace's own package
// keeps `bin/`.
UTest product_directories_are_named_by_package{"WorkspacePlan/ProductDirectoriesAreNamedByPackage", [] {
    const std::vector<mcpp::project::WorkspaceMember> all = {
        listed("apps/cli", "", "cli"),
        listed("ns1/common", "ns1", "common"),
        listed("ns2/common", "ns2", "common"),
    };
    expect(eq(mcpp::project::product_directory_name(all, "apps/cli"), "cli"));
    expect(eq(mcpp::project::product_directory_name(all, "ns1/common"), "ns1.common"));
    expect(eq(mcpp::project::product_directory_name(all, "ns2/common"), "ns2.common"));
    expect(eq(mcpp::project::product_directory_name(all, "."), ""));
}};

// A relative `[indices].path` a member declares was written in the member's
// directory, and one the workspace declares in the workspace's: both are
// anchored where they were written, so two members naming one tree share a
// configuration and a member's path never resolves against the plan's root.
UTest index_paths_are_anchored_where_they_were_written{"WorkspacePlan/IndexPathsAreAnchoredWhereTheyWereWritten", [] {
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
    expect(fatal(root.has_value()));
    auto a = mcpp::project::load_member_manifest(*root, ws, "a");
    auto b = mcpp::project::load_member_manifest(*root, ws, "b");
    if (auto&& failed = expect(fatal(a.has_value())); !failed) failed << a.error();
    if (auto&& failed = expect(fatal(b.has_value())); !failed) failed << b.error();
    expect(eq(a->indices.at("x").path, (ws / "idx").lexically_normal()));
    expect(eq(b->indices.at("x").path, (ws / "idx").lexically_normal()));
    expect(eq(mcpp::project::root_position_key(*a), mcpp::project::root_position_key(*b)));
    std::error_code ec;
    fs::remove_all(ws, ec);
}};

// THE WORKSPACE'S PROFILES ARE ROOT-POSITION VALUES (2026.10.5.2). A member
// inherits each `[profile.<name>]` it does not declare, and its own table of a
// name replaces the workspace's whole, so members without a profile of their
// own share one configuration with the workspace's profile in it.
UTest members_inherit_the_workspaces_profiles_by_name{"WorkspacePlan/MembersInheritTheWorkspacesProfilesByName", [] {
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
    expect(fatal(root.has_value()));
    auto a = mcpp::project::load_member_manifest(*root, ws, "a");
    auto b = mcpp::project::load_member_manifest(*root, ws, "b");
    if (auto&& failed = expect(fatal(a.has_value())); !failed) failed << a.error();
    if (auto&& failed = expect(fatal(b.has_value())); !failed) failed << b.error();
    expect(eq(a->profiles.at("release").optLevel, "3"));
    expect(eq(a->profiles.at("release").ldflags, (std::vector<std::string>{"-Wl,-z,now"})));
    expect(eq(b->profiles.at("release").optLevel, "1"));
    expect(b->profiles.at("release").ldflags.empty());
    expect(eq(b->profiles.at("bench").optLevel, "2"));
    std::error_code ec;
    fs::remove_all(ws, ec);
}};

// THE ROOT PACKAGE OF A WORKSPACE IS A MEMBER (2026.10.5.2): it receives
// `[workspace.build]` once, before its own `[build]`, on every path that reads it.
UTest the_root_package_inherits_the_workspace_build_once{"WorkspacePlan/TheRootPackageInheritsTheWorkspaceBuildOnce", [] {
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
    if (auto&& failed = expect(fatal(effective.has_value())); !failed) failed << effective.error();
    expect(eq(effective->manifest.buildConfig.cxxflags, expected));
    // The manifest a selection reads is the one already inherited; the member
    // "." loaded from it is not inherited a second time.
    auto dot = mcpp::project::load_member_manifest(effective->manifest, ws, ".");
    if (auto&& failed = expect(fatal(dot.has_value())); !failed) failed << dot.error();
    expect(eq(dot->buildConfig.cxxflags, expected));
    auto raw = mcpp::manifest::load(ws / "mcpp.toml");
    expect(fatal(raw.has_value()));
    auto fromRaw = mcpp::project::load_member_manifest(*raw, ws, ".");
    if (auto&& failed = expect(fatal(fromRaw.has_value())); !failed) failed << fromRaw.error();
    expect(eq(fromRaw->buildConfig.cxxflags, expected));
    std::error_code ec;
    fs::remove_all(ws, ec);
}};
