// mcpp.cli.selection — which workspace members a command acts on.
//
// `build`, `test`, `mcpp emit build-database` and `pack` read the same
// selection from the same flags, so the members a command plans are the ones
// the flags name whatever the command is (member selection design 2026-09-30,
// S1). The selection is a SET of members: it is kept in `[workspace] members`
// order whatever order `-p` names them in, and a member named twice is
// selected once, so the plan does not depend on how the command line was
// spelled.
//
// The function is split in two. `select_members(ws, ...)` reads nothing but
// the workspace manifest it is given and its members' manifests, which is what
// the unit tests exercise; the overload that takes a directory finds the
// workspace the directory belongs to and the member the directory is inside,
// and is the one the commands call.

export module mcpp.cli.selection;

import std;
import mcpplibs.cmdline;
import mcpp.manifest;
import mcpp.project;
import mcpp.build.prepare_inputs;

export namespace mcpp::cli {

// The selectors of a command line, as written.
struct MemberRequest {
    bool                     all = false;   // --workspace
    std::vector<std::string> packages;      // every -p/--package, in command-line order
    std::vector<std::string> excludes;      // every --exclude, in command-line order
};

// The workspace a command acts on and the members it selects.
struct MemberSelection {
    std::filesystem::path    root;
    // Each member as `[workspace] members` spells it, in that order; a rooted
    // workspace's own package comes first, as "." (workspace design
    // 2026-09-29 §7.1).
    std::vector<std::string> members;
    // True when the selection is the workspace itself less what `--exclude`
    // removed: `--workspace`, or a virtual root without `-p`. A command that
    // reports per member (`test`) reports in its fan-out form for such a
    // selection even when the workspace has one member.
    bool                     whole = false;
};

// The member path `value` names, spelled as `[workspace] members` spells it
// ("." for a rooted workspace's own package). The value is resolved in the
// order docs/07 §5.3 states, by `mcpp::project::resolve_member_dir`: a
// qualified name, then a package name (refused, naming every match, when
// several members share it), then a directory.
std::expected<std::string, std::string>
member_path_of(const mcpp::manifest::Manifest& ws,
               const std::filesystem::path& wsRoot,
               std::string_view value) {
    auto dir = mcpp::project::resolve_member_dir(ws, wsRoot, value);
    if (!dir) return std::unexpected(dir.error());
    std::string rel = ".";
    if (!dir->empty()) {
        const auto u8 = dir->lexically_normal()
                            .lexically_relative(wsRoot.lexically_normal())
                            .generic_u8string();
        rel.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
        if (rel.empty()) rel = ".";
    }
    // The spelling the manifest uses, so that "./libs/core" and "libs/core"
    // are one member to every reader of the selection.
    for (auto const& mp : ws.workspace.members)
        if (std::filesystem::path(mp).lexically_normal() == std::filesystem::path(rel))
            return mp;
    return rel;
}

// The selection over a workspace whose manifest is `ws` and whose root is
// `wsRoot`. `inside` is the member, as `[workspace] members` spells it, that
// the command's directory is inside; empty at the workspace root.
//
// | request                                  | members                          |
// |------------------------------------------|----------------------------------|
// | `--workspace`                            | every member                     |
// | a virtual root, no `-p`                  | every member                     |
// | a rooted root, no `-p`                   | "."                              |
// | inside member X, no `-p`                 | X                                |
// | `-p X -p Y`                              | {X, Y}                           |
// | an "all" form above, with `--exclude Z`  | every member but Z               |
//
// Refused, before anything is planned: `--workspace` together with `-p`, a
// `-p` that names no member (the refusal lists the members) or several (it
// names every match), `--exclude` together with `-p`, `--exclude` without an
// "all" form, an `--exclude` that names no member, and an `--exclude` that
// removes every member.
std::expected<MemberSelection, std::string>
select_members(const mcpp::manifest::Manifest& ws,
               const std::filesystem::path& wsRoot,
               std::string_view inside,
               const MemberRequest& req) {
    // `--workspace` selects every member and `-p` names some of them. The two
    // together state two selections, and taking either one would drop the
    // other without a word, which is the defect a repeated `-p` had (#750).
    if (req.all && !req.packages.empty())
        return std::unexpected(std::string(
            "--workspace cannot be combined with -p: --workspace selects every member, "
            "and -p names the members a command acts on"));
    if (!req.excludes.empty() && !req.packages.empty())
        return std::unexpected(std::string(
            "--exclude cannot be combined with -p: -p names the members a command acts on, "
            "and --exclude removes members from a whole-workspace selection"));

    const bool rooted = !ws.package.name.empty();
    std::vector<std::string> all;
    if (rooted) all.push_back(".");
    all.insert(all.end(), ws.workspace.members.begin(), ws.workspace.members.end());

    MemberSelection sel{wsRoot, {}, false};
    if (req.all) {
        sel.members = all;
        sel.whole = true;
    } else if (!req.packages.empty()) {
        std::set<std::string> named;
        for (auto const& p : req.packages) {
            auto mp = member_path_of(ws, wsRoot, p);
            if (!mp) return std::unexpected(mp.error());
            named.insert(std::move(*mp));
        }
        // Manifest order, and once each: the selection is a set.
        for (auto const& mp : all)
            if (named.contains(mp)) sel.members.push_back(mp);
    } else if (!inside.empty()) {
        sel.members = {std::string(inside)};
    } else if (!rooted) {
        sel.members = all;
        sel.whole = true;
    } else {
        sel.members = {"."};
    }

    if (sel.members.empty())
        return std::unexpected(std::string("the workspace lists no members"));
    if (req.excludes.empty()) return sel;

    if (!sel.whole)
        return std::unexpected(std::string(
            "--exclude removes members from a whole-workspace selection: "
            "add --workspace (a virtual workspace root selects every member without it)"));
    std::set<std::string> removed;
    for (auto const& e : req.excludes) {
        auto mp = member_path_of(ws, wsRoot, e);
        if (!mp) return std::unexpected(std::format("--exclude '{}': {}", e, mp.error()));
        removed.insert(std::move(*mp));
    }
    std::erase_if(sel.members, [&](const std::string& mp) { return removed.contains(mp); });
    if (sel.members.empty())
        return std::unexpected(std::string(
            "--exclude removes every member of the workspace, so nothing is left to act on"));
    return sel;
}

// The selection for a command run in `cwd`: nullopt outside a workspace (the
// command then acts on the one package it is in). Inside a member's directory
// the workspace is the one that lists the member, and `--workspace` there
// still means the whole of it.
std::expected<std::optional<MemberSelection>, std::string>
select_members(const MemberRequest& req,
               const std::filesystem::path& cwd = std::filesystem::current_path()) {
    auto root = mcpp::project::find_manifest_root(cwd);
    if (!root) return std::optional<MemberSelection>{};
    auto m = mcpp::manifest::load(*root / "mcpp.toml", {.insideWorkspace = true});
    // A manifest that cannot be read is reported by the planner, with its own
    // diagnostic; it is not a selection question.
    if (!m) return std::optional<MemberSelection>{};

    auto outside = [&]() -> std::expected<std::optional<MemberSelection>, std::string> {
        if (!req.excludes.empty())
            return std::unexpected(std::string(
                "--exclude names members of a workspace, and this directory is not in one"));
        return std::optional<MemberSelection>{};
    };

    std::filesystem::path wsRoot = *root;
    std::string inside;
    if (!m->workspace.present) {
        wsRoot = mcpp::project::find_workspace_root(*root);
        if (wsRoot.empty()) return outside();
        m = mcpp::manifest::load(wsRoot / "mcpp.toml");
        if (!m || !m->workspace.present) return outside();
        const auto rel = root->lexically_normal().lexically_relative(wsRoot.lexically_normal());
        for (auto const& mp : m->workspace.members)
            if (std::filesystem::path(mp).lexically_normal() == rel) inside = mp;
        if (inside.empty()) return outside();
    }
    auto sel = select_members(*m, wsRoot, inside, req);
    if (!sel) return std::unexpected(sel.error());
    return std::optional<MemberSelection>{std::move(*sel)};
}


// The selectors of a command line, as `mcpp::cli::select_members` reads them
// (member selection design 2026-09-30, S1): every command that acts on members
// reads its `-p`, `--workspace` and `--exclude` the same way, so the members a
// command plans are the ones the flags name whatever the command is.
MemberRequest member_request(const mcpplibs::cmdline::ParsedArgs& parsed) {
    MemberRequest req;
    req.all = parsed.is_flag_set("workspace");
    req.packages = parsed.option_or_empty("package").values;
    req.excludes = parsed.option_or_empty("exclude").values;
    return req;
}

// The workspace a fan-out acts on, and its members grouped by configuration
// (workspace design 2026-09-29 §15): members whose root-position values are
// equal are planned together, in one graph, in one build directory.
//
// A member's conditional configuration rows are evaluated for the target the
// command builds -- `--target`, else the member's `[build] target`, else the
// host -- with prepare's evaluator, so a group and the plan it becomes agree
// on them by construction (W4).
std::expected<std::vector<std::vector<std::string>>, std::string>
workspace_groups(const std::filesystem::path& wsRoot, const std::vector<std::string>& members,
                 std::string_view targetTriple) {
    auto ws = mcpp::manifest::load(wsRoot / "mcpp.toml");
    if (!ws) return std::unexpected(ws.error().format());
    std::vector<std::vector<std::string>> groups;
    std::map<std::string, std::size_t> byKey;
    for (auto const& mp : members) {
        auto mm = mcpp::project::load_member_manifest(*ws, wsRoot, mp);
        if (!mm) return std::unexpected(mm.error());
        namespace cfgpred = mcpp::build::cfgpred;
        const auto ctx = cfgpred::context_for(
            targetTriple.empty() ? std::string_view(mm->buildConfig.target) : targetTriple);
        const auto key = mcpp::project::root_position_key(*mm, [&](std::string_view predicate) {
            return cfgpred::matches(std::string(predicate), ctx);
        });
        auto [it, fresh] = byKey.try_emplace(key, groups.size());
        if (fresh) groups.emplace_back();
        groups[it->second].push_back(mp);
    }
    return groups;
}

} // namespace mcpp::cli
