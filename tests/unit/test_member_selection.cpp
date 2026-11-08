
import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.cli.selection;

using namespace mcpplibs::mtest;

// Member selection design 2026-09-30, S1 to S3. `select_members` is the one
// function every command that acts on workspace members reads its `-p`,
// `--workspace` and `--exclude` through, so the set a command plans does not
// depend on which command it is, or on the order `-p` names members in. The
// e2e halves (what the selected members build and test) are
// tests/e2e/852_… to 856_….

namespace {

namespace fs = std::filesystem;

struct Workspace {
    fs::path root;

    explicit Workspace(std::string_view tag) {
        root = fs::temp_directory_path()
             / std::format("mcpp-select-{}-{:x}", tag, std::random_device{}());
        fs::create_directories(root);
    }
    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Workspace(const Workspace&) = delete;

    void write(const fs::path& rel, std::string_view text) {
        auto p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }
    void member(std::string_view dir, std::string_view name, std::string_view ns = "") {
        std::string toml = "[package]\n";
        if (!ns.empty()) toml += std::format("namespace = \"{}\"\n", ns);
        toml += std::format("name = \"{}\"\nversion = \"0.1.0\"\n", name);
        write(fs::path(dir) / "mcpp.toml", toml);
    }
    // A virtual workspace root (`[workspace]` alone) or a rooted one.
    mcpp::manifest::Manifest virtual_root(std::string_view members) {
        write("mcpp.toml", std::format("[workspace]\nmembers = [{}]\n", members));
        return load();
    }
    mcpp::manifest::Manifest rooted_root(std::string_view members) {
        write("mcpp.toml", std::format(
            "[package]\nname = \"rootpkg\"\nversion = \"0.1.0\"\n\n[workspace]\nmembers = [{}]\n",
            members));
        return load();
    }
    mcpp::manifest::Manifest load() {
        auto m = mcpp::manifest::load(root / "mcpp.toml");
        if (auto&& failed = expect(m.has_value()); !failed) failed << (m ? "" : m.error().format());
        return m ? std::move(*m) : mcpp::manifest::Manifest{};
    }
};

using Members = std::vector<std::string>;

mcpp::cli::MemberRequest request(bool all, Members packages = {}, Members excludes = {}) {
    return {all, std::move(packages), std::move(excludes)};
}

// Three members in `[workspace] members` order: a, b, c.
struct Three : Workspace {
    Three() : Workspace("three") {
        member("a", "a");
        member("b", "b");
        member("c", "c");
    }
    mcpp::manifest::Manifest ws() { return virtual_root("\"a\", \"b\", \"c\""); }
};

} // namespace

// S1: a virtual root without `-p` is the whole workspace, `--workspace` is
// the whole workspace anywhere, and both are the same set.
UTest a_virtual_root_without_dash_p_and_workspace_are_every_member{"MemberSelection/AVirtualRootWithoutDashPAndWorkspaceAreEveryMember", [] {
    Three f;
    auto ws = f.ws();
    auto implicit = mcpp::cli::select_members(ws, f.root, "", request(false));
    if (auto&& failed = expect(fatal(implicit.has_value())); !failed) failed << implicit.error();
    expect(eq(implicit->members, (Members{"a", "b", "c"})));
    expect(bool(implicit->whole));

    auto all = mcpp::cli::select_members(ws, f.root, "b", request(true));
    if (auto&& failed = expect(fatal(all.has_value())); !failed) failed << all.error();
    expect(eq(all->members, (Members{"a", "b", "c"})));
    expect(bool(all->whole));
}};

// A rooted workspace's own package is "." and comes first. Without a selector
// it is the only member; `--workspace` adds the others.
UTest a_rooted_root_selects_its_own_package_first{"MemberSelection/ARootedRootSelectsItsOwnPackageFirst", [] {
    Workspace f("rooted");
    f.member("a", "a");
    f.member("b", "b");
    auto ws = f.rooted_root("\"a\", \"b\"");

    auto bare = mcpp::cli::select_members(ws, f.root, "", request(false));
    if (auto&& failed = expect(fatal(bare.has_value())); !failed) failed << bare.error();
    expect(eq(bare->members, (Members{"."})));
    expect(!bare->whole);

    auto all = mcpp::cli::select_members(ws, f.root, "", request(true));
    if (auto&& failed = expect(fatal(all.has_value())); !failed) failed << all.error();
    expect(eq(all->members, (Members{".", "a", "b"})));

    auto named = mcpp::cli::select_members(ws, f.root, "", request(false, {"b", "rootpkg"}));
    if (auto&& failed = expect(fatal(named.has_value())); !failed) failed << named.error();
    expect(eq(named->members, (Members{".", "b"})));
}};

// A command run inside a member's directory selects that member.
UTest inside_a_member_selects_that_member{"MemberSelection/InsideAMemberSelectsThatMember", [] {
    Three f;
    auto ws = f.ws();
    auto in = mcpp::cli::select_members(ws, f.root, "b", request(false));
    if (auto&& failed = expect(fatal(in.has_value())); !failed) failed << in.error();
    expect(eq(in->members, (Members{"b"})));
    expect(!in->whole);
}};

// S1: `-p` names members, each resolved in the order docs/07 §5.3 states. The
// selection is a set kept in manifest order: the order `-p` was written in
// does not change it, and a member named twice, by two spellings, is one.
UTest several_dash_p_select_their_members_once_in_manifest_order{"MemberSelection/SeveralDashPSelectTheirMembersOnceInManifestOrder", [] {
    Workspace f("set");
    f.member("libs/a", "alpha");
    f.member("libs/b", "beta");
    f.member("libs/c", "gamma");
    auto ws = f.virtual_root("\"libs/a\", \"libs/b\", \"libs/c\"");

    auto ab = mcpp::cli::select_members(ws, f.root, "", request(false, {"alpha", "beta"}));
    auto ba = mcpp::cli::select_members(ws, f.root, "", request(false, {"beta", "alpha"}));
    if (auto&& failed = expect(fatal(ab.has_value())); !failed) failed << ab.error();
    if (auto&& failed = expect(fatal(ba.has_value())); !failed) failed << ba.error();
    expect(eq(ab->members, (Members{"libs/a", "libs/b"})));
    expect(eq(ab->members, ba->members));
    expect(!ab->whole);

    // The package name, the member path and the directory's last segment are
    // three spellings of one member.
    auto same = mcpp::cli::select_members(ws, f.root, "",
                                          request(false, {"alpha", "libs/a", "a", "./libs/a"}));
    if (auto&& failed = expect(fatal(same.has_value())); !failed) failed << same.error();
    expect(eq(same->members, (Members{"libs/a"})));
}};

// A `-p` that names no member is refused, and the refusal names it and lists
// the members; one that several members match is refused naming every match.
UTest an_unknown_or_ambiguous_dash_p_is_refused_by_name{"MemberSelection/AnUnknownOrAmbiguousDashPIsRefusedByName", [] {
    Workspace f("refuse");
    f.member("a", "same", "ns1");
    f.member("b", "same", "ns2");
    f.member("c", "other");
    auto ws = f.virtual_root("\"a\", \"b\", \"c\"");

    auto unknown = mcpp::cli::select_members(ws, f.root, "", request(false, {"c", "nosuch"}));
    expect(fatal(!unknown.has_value()));
    if (auto&& failed = expect(ne(unknown.error().find("nosuch"), std::string::npos)); !failed) failed << unknown.error();
    if (auto&& failed = expect(ne(unknown.error().find("'c'"), std::string::npos)); !failed) failed << unknown.error();

    auto ambiguous = mcpp::cli::select_members(ws, f.root, "", request(false, {"same"}));
    expect(fatal(!ambiguous.has_value()));
    if (auto&& failed = expect(ne(ambiguous.error().find("ns1.same"), std::string::npos)); !failed) failed << ambiguous.error();
    if (auto&& failed = expect(ne(ambiguous.error().find("ns2.same"), std::string::npos)); !failed) failed << ambiguous.error();

    auto qualified = mcpp::cli::select_members(ws, f.root, "",
                                               request(false, {"ns2.same", "ns1.same"}));
    if (auto&& failed = expect(fatal(qualified.has_value())); !failed) failed << qualified.error();
    expect(eq(qualified->members, (Members{"a", "b"})));
}};

// S3: `--exclude` removes members from either "all" form, by the same
// resolution as `-p`, and keeps manifest order.
UTest exclude_removes_members_from_an_all_form{"MemberSelection/ExcludeRemovesMembersFromAnAllForm", [] {
    Three f;
    auto ws = f.ws();
    auto flag = mcpp::cli::select_members(ws, f.root, "", request(true, {}, {"c"}));
    if (auto&& failed = expect(fatal(flag.has_value())); !failed) failed << flag.error();
    expect(eq(flag->members, (Members{"a", "b"})));
    expect(bool(flag->whole));

    // The implicit whole selection of a virtual root, and two spellings.
    auto implicit = mcpp::cli::select_members(ws, f.root, "", request(false, {}, {"a", "./c"}));
    if (auto&& failed = expect(fatal(implicit.has_value())); !failed) failed << implicit.error();
    expect(eq(implicit->members, (Members{"b"})));

    Workspace r("rooted-exclude");
    r.member("a", "a");
    auto rws = r.rooted_root("\"a\"");
    auto root = mcpp::cli::select_members(rws, r.root, "", request(true, {}, {"rootpkg"}));
    if (auto&& failed = expect(fatal(root.has_value())); !failed) failed << root.error();
    expect(eq(root->members, (Members{"a"})));
}};

// S1, refusals: before anything is planned, `--exclude` with `-p`, `--exclude`
// that names no member, `--exclude` that leaves nothing, and `--exclude`
// where no "all" form applies.
UTest exclude_is_refused_where_it_has_no_meaning{"MemberSelection/ExcludeIsRefusedWhereItHasNoMeaning", [] {
    Three f;
    auto ws = f.ws();

    auto withP = mcpp::cli::select_members(ws, f.root, "", request(false, {"a"}, {"b"}));
    expect(fatal(!withP.has_value()));
    if (auto&& failed = expect(ne(withP.error().find("--exclude"), std::string::npos)); !failed) failed << withP.error();
    if (auto&& failed = expect(ne(withP.error().find("-p"), std::string::npos)); !failed) failed << withP.error();

    auto withPAndAll = mcpp::cli::select_members(ws, f.root, "", request(true, {"a"}, {"b"}));
    expect(!withPAndAll.has_value());

    auto unknown = mcpp::cli::select_members(ws, f.root, "", request(true, {}, {"nosuch"}));
    expect(fatal(!unknown.has_value()));
    if (auto&& failed = expect(ne(unknown.error().find("nosuch"), std::string::npos)); !failed) failed << unknown.error();

    auto everything = mcpp::cli::select_members(ws, f.root, "", request(true, {}, {"a", "b", "c"}));
    expect(fatal(!everything.has_value()));
    if (auto&& failed = expect(ne(everything.error().find("every member"), std::string::npos)); !failed) failed << everything.error();

    // Inside a member, and at a rooted root, without `--workspace`: nothing is
    // a whole-workspace selection to remove from.
    auto inside = mcpp::cli::select_members(ws, f.root, "a", request(false, {}, {"b"}));
    expect(fatal(!inside.has_value()));
    if (auto&& failed = expect(ne(inside.error().find("--workspace"), std::string::npos)); !failed) failed << inside.error();

    Workspace r("rooted-refuse");
    r.member("a", "a");
    auto rws = r.rooted_root("\"a\"");
    expect(!(mcpp::cli::select_members(rws, r.root, "", request(false, {}, {"a"})).has_value()));
}};

// `--workspace` and `-p` state two selections; neither is taken over the other.
UTest workspace_together_with_dash_p_is_refused{"MemberSelection/WorkspaceTogetherWithDashPIsRefused", [] {
    Three f;
    auto ws = f.ws();
    auto both = mcpp::cli::select_members(ws, f.root, "", request(true, {"a"}, {}));
    expect(fatal(!both.has_value()));
    if (auto&& failed = expect(ne(both.error().find("--workspace"), std::string::npos)); !failed) failed << both.error();
    if (auto&& failed = expect(ne(both.error().find("-p"), std::string::npos)); !failed) failed << both.error();
}};

// The overload that takes a directory finds the workspace the directory
// belongs to: from a member's directory, `-p` and `--workspace` still select
// among the members of the workspace that lists it. Outside a workspace there
// is no selection, and `--exclude` has nothing to name.
UTest the_directory_decides_the_workspace_and_the_member_it_is_in{"MemberSelection/TheDirectoryDecidesTheWorkspaceAndTheMemberItIsIn", [] {
    Three f;
    f.ws();

    auto at_root = mcpp::cli::select_members(request(false), f.root);
    if (auto&& failed = expect(fatal(at_root.has_value())); !failed) failed << at_root.error();
    expect(fatal(at_root->has_value()));
    expect(eq((*at_root)->members, (Members{"a", "b", "c"})));

    auto in_member = mcpp::cli::select_members(request(false), f.root / "b");
    if (auto&& failed = expect(fatal(in_member.has_value())); !failed) failed << in_member.error();
    expect(fatal(in_member->has_value()));
    expect(eq((*in_member)->members, (Members{"b"})));
    expect(eq(std::filesystem::weakly_canonical((*in_member)->root), std::filesystem::weakly_canonical(f.root)));

    auto several_from_member = mcpp::cli::select_members(request(false, {"a", "c"}), f.root / "b");
    if (auto&& failed = expect(fatal(several_from_member.has_value())); !failed) failed << several_from_member.error();
    expect(fatal(several_from_member->has_value()));
    expect(eq((*several_from_member)->members, (Members{"a", "c"})));

    auto all_from_member = mcpp::cli::select_members(request(true, {}, {"a"}), f.root / "b");
    if (auto&& failed = expect(fatal(all_from_member.has_value())); !failed) failed << all_from_member.error();
    expect(fatal(all_from_member->has_value()));
    expect(eq((*all_from_member)->members, (Members{"b", "c"})));

    Workspace alone("alone");
    alone.member(".", "solo");
    auto none = mcpp::cli::select_members(request(false), alone.root);
    if (auto&& failed = expect(fatal(none.has_value())); !failed) failed << none.error();
    expect(!none->has_value());
    auto excluded = mcpp::cli::select_members(request(false, {}, {"x"}), alone.root);
    expect(fatal(!excluded.has_value()));
    if (auto&& failed = expect(ne(excluded.error().find("not in one"), std::string::npos)); !failed) failed << excluded.error();
}};
