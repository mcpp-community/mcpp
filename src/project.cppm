// mcpp.project — project/workspace location + workspace-dependency merging.
//
// Shared by the CLI layer and the pm subsystem (which previously kept a
// private copy of find_manifest_root to avoid importing mcpp.cli).
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.project;

import std;
import mcpp.manifest;
import mcpp.ui;

namespace mcpp::project {

// Locate mcpp.toml by walking upward from cwd.
export std::optional<std::filesystem::path> find_manifest_root(std::filesystem::path start) {
    auto p = std::filesystem::absolute(start);
    while (true) {
        if (std::filesystem::exists(p / "mcpp.toml")) return p;
        auto parent = p.parent_path();
        if (parent == p) return std::nullopt;
        p = parent;
    }
}

// Find the workspace root by walking upward from a member directory.
// Returns empty if no workspace root found.
export std::filesystem::path find_workspace_root(const std::filesystem::path& memberRoot) {
    auto p = memberRoot.parent_path();
    while (true) {
        if (std::filesystem::exists(p / "mcpp.toml")) {
            auto m = mcpp::manifest::load(p / "mcpp.toml");
            if (m && m->workspace.present) {
                // Verify memberRoot is in members list
                auto rel = std::filesystem::relative(memberRoot, p);
                for (auto& member : m->workspace.members) {
                    if (rel == std::filesystem::path(member)) return p;
                }
            }
        }
        auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return {};
}

// Merge workspace.dependencies into a member's deps (`x.workspace = true`).
//
// #224: this used to propagate only `version`, so a workspace-level
// `[workspace.dependencies] x = { path = "..." }` inherited by a member was
// silently treated as a version/index dep (empty version) and failed to
// resolve. Now the location fields (path/git/*) travel too — a dep spec is
// one of {version, path, git} so copying whichever the workspace declared
// is correct.
//
// `wsRoot` anchors a relative `path`: the workspace author wrote it
// relative to the WORKSPACE ROOT (where `[workspace.dependencies]` lives),
// not the inheriting member's own directory, so it is resolved to an
// absolute path here — downstream path-dep resolution (relative to the
// member root) then sees an already-absolute path and leaves it alone.
export void merge_workspace_deps(mcpp::manifest::Manifest& member,
                          const mcpp::manifest::Manifest& workspace,
                          const std::filesystem::path& wsRoot = {}) {
    auto copy_from = [&](mcpp::manifest::DependencySpec& spec,
                         const mcpp::manifest::DependencySpec& wsSpec) {
        spec.version    = wsSpec.version;
        spec.path       = wsSpec.path;
        spec.git        = wsSpec.git;
        spec.gitRev     = wsSpec.gitRev;
        spec.gitRefKind = wsSpec.gitRefKind;
        if (!spec.path.empty() && !wsRoot.empty()) {
            std::filesystem::path p(spec.path);
            if (p.is_relative()) {
                spec.path = std::filesystem::weakly_canonical(wsRoot / p).string();
            }
        }
        spec.inheritWorkspace = false;
    };
    auto merge_map = [&](std::map<std::string, mcpp::manifest::DependencySpec>& deps) {
        for (auto& [name, spec] : deps) {
            if (!spec.inheritWorkspace) continue;
            // Try exact key match first
            auto it = workspace.workspace.dependencies.find(name);
            if (it != workspace.workspace.dependencies.end()) {
                copy_from(spec, it->second);
                continue;
            }
            // Try short name for default-ns deps
            auto shortIt = workspace.workspace.dependencies.find(spec.shortName);
            if (shortIt != workspace.workspace.dependencies.end()) {
                copy_from(spec, shortIt->second);
            }
        }
    };
    merge_map(member.dependencies);
    merge_map(member.devDependencies);
    merge_map(member.buildDependencies);
}

// Inherit the workspace root's `[indices]` when the member declares none.
// A relative `[indices].path` was written at the WORKSPACE ROOT, so it must
// resolve against `wsRoot` and not the member directory — otherwise every
// member needs its own `../`-prefixed copy of the same declaration (#224).
//
// Shared so every reader of `[indices]` sees the same effective map: the build
// path resolves dependencies through it, and `mcpp add` decides whether a
// package exists through it. Two copies of this rule is how the two ended up
// disagreeing about which packages are real.
export void inherit_workspace_indices(mcpp::manifest::Manifest& member,
                                      const mcpp::manifest::Manifest& workspace,
                                      const std::filesystem::path& wsRoot) {
    if (!member.indices.empty() || workspace.indices.empty()) return;
    member.indices = workspace.indices;
    for (auto& [_, idx] : member.indices) {
        if (idx.is_local() && idx.path.is_relative()) {
            // This is an ownership/anchoring operation, not a request to
            // resolve filesystem aliases.  weakly_canonical can rewrite a
            // Windows short/case-preserving workspace path into a different
            // spelling before the inherited index is opened.  Keep the path
            // rooted exactly where the workspace manifest declared it; the
            // normal reader remains responsible for existence/readability.
            idx.path = (wsRoot / idx.path).lexically_normal();
        }
    }
}

// Is `candidate` one of this workspace's declared members?
//
// The membership test is the workspace's OWN `members` list resolved against
// the workspace root — not "is this path under the workspace directory". A
// `path` dependency can live inside the tree without being a member (a vendored
// copy, an example, a scratch package), and a member's flags are exactly what
// it must not acquire.
export bool is_workspace_member(const mcpp::manifest::Manifest& workspace,
                                const std::filesystem::path& wsRoot,
                                const std::filesystem::path& candidate) {
    if (!workspace.workspace.present) return false;
    std::error_code ec;
    auto want = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) { ec.clear(); want = candidate.lexically_normal(); }
    for (auto const& m : workspace.workspace.members) {
        auto member = std::filesystem::weakly_canonical(wsRoot / m, ec);
        if (ec) { ec.clear(); member = (wsRoot / m).lexically_normal(); }
        if (member == want) return true;
    }
    return false;
}

export void inherit_workspace_build(mcpp::manifest::Manifest& member,
                                    const mcpp::manifest::Manifest& workspace,
                                    const std::filesystem::path& wsRoot);

// The `[workspace.package]` half on its own — metadata, no paths, so no anchor
// argument. Second caller: a member reached as a sibling's `path` dependency,
// which may legally omit `version` because this table supplies it.
export void inherit_workspace_package(mcpp::manifest::Manifest& member,
                                      const mcpp::manifest::Manifest& workspace) {
    const auto& inh = workspace.workspace.inherited;
    // `standardDeclared` and not `standard != "c++23"`: a member that
    // deliberately pins c++23 under a c++26 workspace must keep it, and that is
    // indistinguishable from the default without the bit.
    if (inh.standardDeclared && !member.package.standardDeclared) {
        member.package.standard  = inh.standard;
        member.language.standard = inh.standard;
        member.package.standardDeclared = true;
        // `cppStandard` was normalised by the parser from the member's own
        // value; it has to be re-derived, or the inherited spelling would sit
        // in `package.standard` while every build surface kept reading the
        // default out of the normalised copy.
        if (auto cfg = mcpp::manifest::normalize_cpp_standard(inh.standard))
            member.cppStandard = *cfg;
    }
    if (member.package.version.empty())     member.package.version     = inh.version;
    if (member.package.license.empty())     member.package.license     = inh.license;
    if (member.package.description.empty()) member.package.description = inh.description;
    if (member.package.repo.empty())        member.package.repo        = inh.repo;
    if (member.package.authors.empty())     member.package.authors     = inh.authors;
    if (member.package.mcppFloor.empty())   member.package.mcppFloor   = inh.mcppFloor;
}

// EVERYTHING A MEMBER INHERITS FROM ITS WORKSPACE ROOT, IN ONE FUNCTION.
//
// There are two inheritance SITES in prepare_build — the command issued at the
// workspace root with `-p <member>`, and the command issued inside a member
// directory — and until this function existed they were two hand-copied lists
// of the same merges. A fifth key added to one of them is a defect that
// compiles, which is exactly how `[build]` came to be inherited by neither
// (#527 Bug 2).
//
// The discipline is stated on `WorkspaceInherited`: scalars are taken when the
// member did not DECLARE the key, vectors append with the workspace first, and
// dependencies keep their explicit `.workspace = true` opt-in because they are
// graph edges. `[toolchain]`, `[target.<triple>]` and `[indices]` were already
// inherited before these tables existed and keep the behaviour they had.
//
// `wsRoot` anchors relative paths: an `[indices].path` or a
// `[workspace.dependencies] path` was written against the WORKSPACE ROOT, and
// re-anchoring it to the member directory is #224.
// The workspace root's `[xlings.workspace]`, for a member (#713).
//
// An xlings entry describes the environment a build runs in, the same class of
// declaration as `[toolchain]` and `[target.*]`, so it is inherited implicitly
// rather than through an explicit opt-in: only dependencies, which are graph
// edges, need `.workspace = true` (`merge_workspace_deps`). Before this a member
// saw none of the root's entries: they were installed for the workspace, and
// `mcpp::xpkg_dir` in the member's build program still answered "" for them.
//
// The root's entries come first and a member's own declaration of the same
// package wins, which is the "nearer the artifact" rule of SPEC-004 §4.5 --
// identity is `(namespace, name)`, decided by `package_key`. Conditional
// `[target.<sel>.xlings.workspace]` rows travel as conditional rows, so they are
// still decided by the selector at merge time. Feature-gated entries do not
// travel: a feature belongs to the package that declares it. Neither does the
// emitter's per-platform view (`workspaceByPlatform`), so a published member's
// descriptor states only what the member itself declared; the `subos` is the
// root's choice already (`select_runtime`).
export void inherit_workspace_xlings(mcpp::manifest::Manifest& member,
                                     const mcpp::manifest::Manifest& workspace) {
    // `(namespace, name)`, the identity `mcpp.xlings.address_set` defines,
    // spelled here from the same parser rather than imported. Importing that
    // module here makes GCC 16.1 fail with an internal compiler error
    // (segmentation fault) at `import mcpp.cli;` in src/main.cpp. Measured.
    auto package_key = [](std::string_view address) {
        const auto e = mcpp::manifest::parse_address(address);
        return (e.ns.empty() ? std::string("xim") : e.ns) + ":" + e.target;
    };
    std::set<std::string> own;
    for (auto const& a : member.xlings.deps) own.insert(package_key(a));
    for (auto const& cc : member.conditionalConfigs)
        for (auto const& a : cc.xlings.deps) own.insert(package_key(a));

    // Copies the entries of `from` whose package the member does not declare,
    // with the pin and the tier each address carries.
    auto take = [&](const mcpp::manifest::XlingsConfig& from,
                    mcpp::manifest::XlingsConfig& to) {
        std::vector<std::string> taken;
        for (auto const& a : from.deps) {
            if (own.contains(package_key(a))) continue;
            taken.push_back(a);
            const auto target = mcpp::manifest::parse_address(a).target;
            if (auto pin = from.workspace.find(target); pin != from.workspace.end())
                to.workspace.try_emplace(pin->first, pin->second);
            if (auto w = from.depWhen.find(a); w != from.depWhen.end())
                to.depWhen.try_emplace(a, w->second);
            if (from.onRequest.contains(a)) to.onRequest.insert(a);
        }
        to.deps.insert(to.deps.begin(), taken.begin(), taken.end());
        // `[xlings.overrides]` states where a payload comes from on THIS
        // machine, so a member built as the root of its own build takes the
        // workspace's statement unless it makes its own (mcpp#755).
        for (auto const& [pkg, o] : from.overrides) to.overrides.try_emplace(pkg, o);
    };
    take(workspace.xlings, member.xlings);

    std::vector<mcpp::manifest::ConditionalConfig> rows;
    for (auto const& cc : workspace.conditionalConfigs) {
        if (cc.xlings.deps.empty() && cc.xlings.overrides.empty()) continue;
        mcpp::manifest::ConditionalConfig row;
        row.predicate = cc.predicate;
        take(cc.xlings, row.xlings);
        if (!row.xlings.deps.empty() || !row.xlings.overrides.empty())
            rows.push_back(std::move(row));
    }
    member.conditionalConfigs.insert(member.conditionalConfigs.begin(),
                                     std::make_move_iterator(rows.begin()),
                                     std::make_move_iterator(rows.end()));
}

// The keys a member inherits only where it is the ROOT of a build: `[toolchain]`,
// `[target.<triple>]`, `[indices]` and `[profile.<name>]`. They choose the
// compiler, the target rows, the indices and the build profile for the whole
// graph, so a member reached as somebody's dependency takes them from that
// build's root instead. A member built as a
// host tool is the root of its own sub-build, which is the second caller
// (#710): without it, `mcpp build -p tool` used the workspace's compiler and
// the same tool built for a consumer used the global default.
export void inherit_workspace_root_position(mcpp::manifest::Manifest& member,
                                            const mcpp::manifest::Manifest& workspace,
                                            const std::filesystem::path& wsRoot) {
    if (member.toolchain.byPlatform.empty())
        member.toolchain = workspace.toolchain;
    for (auto& [triple, entry] : workspace.targetOverrides)
        if (!member.targetOverrides.contains(triple))
            member.targetOverrides[triple] = entry;
    // A profile is inherited by name, and a member's own table of that name
    // replaces it whole, as `[target.<triple>]` does. Before 2026.10.5.2 the
    // workspace's profiles reached no member: a virtual root's were ignored
    // without a word, and a rooted root's applied only when its own package
    // was the first one selected.
    for (auto& [name, profile] : workspace.profiles)
        if (!member.profiles.contains(name))
            member.profiles[name] = profile;
    inherit_workspace_indices(member, workspace, wsRoot);
}

// THE ROOT PACKAGE OF A WORKSPACE IS ONE OF ITS MEMBERS.
//
// A root that carries `[package]` beside `[workspace]` is selected as member
// `"."`, and it receives `[workspace.package]` and `[workspace.build]` once,
// as every other member does, so that one manifest compiles the same way in
// every selection. Its root-position keys and `[workspace.dependencies]` are
// handled by the caller: they are its own already, or merged explicitly.
export void inherit_as_root_package(mcpp::manifest::Manifest& root,
                                    const std::filesystem::path& wsRoot) {
    if (!root.workspace.present || root.package.name.empty()) return;
    if (root.inheritedAsRootPackage) return;
    root.inheritedAsRootPackage = true;
    const auto workspace = root;
    inherit_workspace_package(root, workspace);
    inherit_workspace_build(root, workspace, wsRoot);
}

export void inherit_workspace_config(mcpp::manifest::Manifest& member,
                                     const mcpp::manifest::Manifest& workspace,
                                     const std::filesystem::path& wsRoot) {
    merge_workspace_deps(member, workspace, wsRoot);
    inherit_workspace_root_position(member, workspace, wsRoot);
    inherit_workspace_xlings(member, workspace);

    // The two halves, each with a second caller of its own: a member reached
    // as a sibling.s `path` dependency needs both, at two different points.
    // (The "still missing after inheritance" refusal is
    //  `workspace_inheritance_error`, called by each site.)
    inherit_workspace_package(member, workspace);
    inherit_workspace_build(member, workspace, wsRoot);
}

// The `[workspace.build]` half on its own.
//
// SEPARATE BECAUSE IT HAS A SECOND CALLER. `inherit_workspace_config` runs for
// the manifest the command names; this runs additionally for every OTHER member
// pulled in as a `path` dependency — which is what workspace members are to each
// other, and therefore the ordinary case rather than an exotic one. Without the
// second call, `mcpp build -p appb` gave `appb` the workspace flags and gave the
// sibling `liba` none, while compiling both in the same command.
//
// `[workspace.package] standard` needs no second call: the standard is imposed
// graph-wide from the root for BMI-compatibility reasons, which is precisely
// why this gap stayed invisible until a `[build]` key became inheritable too.
export void inherit_workspace_build(mcpp::manifest::Manifest& member,
                                    const mcpp::manifest::Manifest& workspace,
                                    const std::filesystem::path& wsRoot) {
    const auto& inh = workspace.workspace.inherited;
    if (!inh.buildPresent) return;
    auto& b = member.buildConfig;
    const auto& w = inh.build;
    {
        auto prepend = [](auto& dst, const auto& src) {
            if (src.empty()) return;
            dst.insert(dst.begin(), src.begin(), src.end());
        };
        prepend(b.cflags,   w.cflags);
        prepend(b.cxxflags, w.cxxflags);
        prepend(b.ldflags,  w.ldflags);
        prepend(b.defines,  w.defines);
        prepend(b.dialectCxxflags, w.dialectCxxflags);
        // Where each inherited value was written, for the diagnostics that
        // name a key (WS3): a word the workspace states is the workspace's.
        for (auto const& [key, src] : {
                 std::pair{"cflags", &w.cflags}, std::pair{"cxxflags", &w.cxxflags},
                 std::pair{"ldflags", &w.ldflags}, std::pair{"defines", &w.defines},
                 std::pair{"dialect_cxxflags", &w.dialectCxxflags}}) {
            if (src->empty()) continue;
            auto& rec = b.inheritedFromWorkspace[key];
            rec.insert(rec.end(), src->begin(), src->end());
        }
        // A RELATIVE INCLUDE DIRECTORY IN THE WORKSPACE MANIFEST WAS WRITTEN
        // AGAINST THE WORKSPACE ROOT, and every member would otherwise resolve
        // it against its own directory.
        //
        // This is #224 for a new key: `[indices].path` and
        // `[workspace.dependencies] path` are anchored for exactly this reason,
        // and a third relative-path key that skipped it would silently point at
        // `<member>/shared/inc` for a directory that lives at
        // `<workspace>/shared/inc`. The failure is a missing header, three
        // members deep, naming neither the manifest that declared it nor the
        // root it was declared against.
        //
        // Anchored rather than refused: an absolute include directory is
        // already accepted by `expandIncludeDirs`, so the anchored form needs
        // no new handling downstream.
        auto anchored = [&](const std::vector<std::filesystem::path>& src) {
            std::vector<std::filesystem::path> out;
            out.reserve(src.size());
            for (auto const& d : src)
                out.push_back(d.is_absolute() ? d
                                              : (wsRoot / d).lexically_normal());
            return out;
        };
        prepend(b.includeDirs,        anchored(w.includeDirs));
        prepend(b.includeDirsAfter,   anchored(w.includeDirsAfter));
        prepend(b.privateIncludeDirs, anchored(w.privateIncludeDirs));
        if (b.cStandard.empty())            b.cStandard            = w.cStandard;
        if (b.linkage.empty())              b.linkage              = w.linkage;
        if (b.target.empty())               b.target               = w.target;
        if (b.cxxRuntime.empty())           b.cxxRuntime           = w.cxxRuntime;
        if (b.dependencyLinkage.empty())    b.dependencyLinkage    = w.dependencyLinkage;
        if (b.macosDeploymentTarget.empty())
            b.macosDeploymentTarget = w.macosDeploymentTarget;
        if (b.iosDeploymentTarget.empty())
            b.iosDeploymentTarget = w.iosDeploymentTarget;
    }
}

// The required-field check, asked at the one point where it is answerable.
//
// `mcpp.manifest`'s parser cannot enforce `package.name` / `package.version` on
// a workspace member, because a member manifest carries no evidence that it is
// one. Deferring the check is not relaxing it: it runs here, after inheritance,
// and the message can name the workspace table that would have supplied the
// value — which the parser could not have done either.
export std::optional<std::string> workspace_inheritance_error(
    const mcpp::manifest::Manifest& member,
    const std::filesystem::path& memberDir) {
    auto missing = [&](std::string_view field, std::string_view wsKey)
        -> std::optional<std::string> {
        return std::format(
            "{}: missing required field '{}', and the workspace root does not "
            "supply it either.\n"
            "       Declare it in the member, or once for every member:\n"
            "\n"
            "         [workspace.package]\n"
            "         {} = \"...\"",
            (memberDir / "mcpp.toml").string(), field, wsKey);
    };
    if (member.package.name.empty())
        return std::format("{}: missing required field 'package.name'. "
                           "A workspace cannot supply it: members do not share "
                           "a name.", (memberDir / "mcpp.toml").string());
    if (member.package.version.empty()) return missing("package.version", "version");
    return std::nullopt;
}

// A `x.workspace = true` entry that no workspace resolved (#714).
//
// Inheritance replaces the entry with the workspace's declaration; an entry
// still marked afterwards names nothing. It used to fall through as a version
// dependency with an empty version, refused far downstream as "SemVer
// constraint '' ... run `mcpp index update`" -- an instruction about the index
// for a mistake in the manifest. Asked once, after inheritance, at each place a
// manifest enters a build: the root, a member built with `-p`, and every
// dependency load site. The two ways out are the two ways inheritance happens.
export std::optional<std::string>
unresolved_workspace_dependency_error(const mcpp::manifest::Manifest& m,
                                      const std::filesystem::path& manifestDir) {
    auto first = [](const std::map<std::string, mcpp::manifest::DependencySpec>& deps)
        -> std::optional<std::string> {
        for (auto const& [name, spec] : deps)
            if (spec.inheritWorkspace) return name;
        return std::nullopt;
    };
    std::optional<std::string> name;
    std::string_view table;
    if ((name = first(m.dependencies)))           table = "dependencies";
    else if ((name = first(m.devDependencies)))   table = "dev-dependencies";
    else if ((name = first(m.buildDependencies))) table = "build-dependencies";
    if (!name) return std::nullopt;
    return std::format(
        "{}: [{}] {} = {{ workspace = true }}, but no workspace declares '{}'.\n"
        "       `workspace = true` is resolved against the [workspace.dependencies] "
        "of the workspace whose `members` list this package.\n"
        "       fix: list this package in that workspace's [workspace] members, "
        "or state the dependency's version, path or git source here.",
        (manifestDir / "mcpp.toml").string(), table, *name, *name);
}

// THE EFFECTIVE MANIFEST OF A PROJECT DIRECTORY, FOR EVERY READER OUTSIDE
// `prepare_build`.
//
// `prepare_build` applies workspace inheritance where it loads the manifest a
// command names. Every other command that reads a project manifest (publish,
// pack routing, `emit xpkg`, `toolchain list`) used to call `manifest::load`
// directly and therefore saw the raw file: a member that omits `version`
// because `[workspace.package]` supplies it was refused, and a member without
// `[toolchain]` was reported against the global default while `mcpp build` in
// the same directory resolved the workspace's toolchain (#690, F5a and F6).
//
// The rule is the one `prepare_build` follows: a directory that its workspace
// lists as a member is loaded with `insideWorkspace` and receives
// `inherit_workspace_config` anchored at the workspace root, and the
// required-field check runs after inheritance. A workspace root that carries
// its own `[package]` receives the same inheritance as member `"."`
// (`inherit_as_root_package`). Any other directory is loaded as written.
export struct EffectiveManifest {
    mcpp::manifest::Manifest                manifest;       // after inheritance
    std::optional<mcpp::manifest::Manifest> workspace;      // set when `member`
    std::filesystem::path                   workspaceRoot;  // empty unless `member`
    bool                                    member = false;
};

export std::expected<EffectiveManifest, std::string>
load_effective_manifest(const std::filesystem::path& dir) {
    const auto manifestPath = dir / "mcpp.toml";
    const auto wsRoot = find_workspace_root(std::filesystem::absolute(dir));
    if (wsRoot.empty()) {
        auto m = mcpp::manifest::load(manifestPath);
        if (!m) return std::unexpected(m.error().format());
        inherit_as_root_package(*m, std::filesystem::absolute(dir));
        return EffectiveManifest{ std::move(*m), std::nullopt, {}, false };
    }
    auto m = mcpp::manifest::load(manifestPath, {.insideWorkspace = true});
    if (!m) return std::unexpected(m.error().format());
    auto ws = mcpp::manifest::load(wsRoot / "mcpp.toml");
    if (!ws) return std::unexpected(ws.error().format());
    inherit_workspace_config(*m, *ws, wsRoot);
    if (auto bad = workspace_inheritance_error(*m, dir))
        return std::unexpected(*bad);
    return EffectiveManifest{ std::move(*m), std::move(*ws), wsRoot, true };
}

// One declared workspace member, with the identity its own manifest states.
//
// #725: `-p, --package <NAME>` promises a package (SPEC-001's `(namespace,
// name)`), so resolving it needs each member's OWN declaration, not only the
// directory `[workspace] members` names it with.
export struct WorkspaceMember {
    std::string            memberPath;  // as written in [workspace] members
    std::filesystem::path  dir;         // rootDir / memberPath
    std::string            namespace_;  // "" when undeclared, or unreadable
    std::string            name;        // "" when the manifest could not be
                                        // read -- such a member still matches
                                        // by directory or path (step 3 below)
};

// A member's qualified name the way SPEC-001 spells one: `<namespace>.<name>`.
// Meaningless (and not attempted by the resolver below) when the member
// declares no namespace -- SPEC-001 §3.1: an unnamespaced package's bare name
// already IS its full identity, so there is no separate dotted spelling to
// match against.
export std::string qualified_member_name(const WorkspaceMember& m) {
    return m.namespace_.empty() ? m.name : m.namespace_ + "." + m.name;
}

// Every declared member, with its own package identity. Loaded the way a
// member is always loaded (`insideWorkspace = true`, so an omitted
// `package.version` is not refused here); a member whose manifest fails to
// parse keeps an empty name rather than aborting the listing, because `-p`
// still owes an answer about the members that DO parse.
export std::vector<WorkspaceMember>
workspace_members(const mcpp::manifest::Manifest& rootManifest,
                  const std::filesystem::path& rootDir) {
    std::vector<WorkspaceMember> out;
    out.reserve(rootManifest.workspace.members.size());
    for (auto& mp : rootManifest.workspace.members) {
        WorkspaceMember wm;
        wm.memberPath = mp;
        wm.dir = rootDir / mp;
        if (auto mm = mcpp::manifest::load(wm.dir / "mcpp.toml", {.insideWorkspace = true})) {
            wm.namespace_ = mm->package.namespace_;
            wm.name       = mm->package.name;
        }
        out.push_back(std::move(wm));
    }
    return out;
}

// Resolve which member directory a workspace command acts on, for the
// single-member case. Shared by every `-p`/`--package` reader (build, test,
// run, emit, and prepare_build's own root-level switch, #725), so
// `build -p X` and `test -p X` always agree on what X means.
//
// A value is resolved in this order, because the option names a package
// (SPEC-001) and a directory is a fallback spelling docs/07 §5.3 has always
// documented:
//   1. a member's qualified name, `<namespace>.<name>` (only attempted for a
//      member that declares a namespace -- see `qualified_member_name`);
//   2. otherwise, a member's bare package name -- refused, naming every
//      match's qualified name, when two or more members share it;
//   3. otherwise, a member's path as written in `[workspace] members`, or its
//      directory's last segment. Two members sharing a directory basename
//      keep today's first-match selection (a script written against it keeps
//      working), with a warning naming the others and their paths.
// A value that is one member's package name (step 2) and a different
// member's directory (step 3) selects the step-2 member, with a warning
// naming the other member and its path.
//
// Returns:
//   - the member dir   when `package_filter` names a member,
//   - empty path       when no switch applies (not a workspace, or a rooted
//                      workspace with no filter → act on the root package),
//   - error            when the filter names no member, names more than one,
//                      or a *virtual* workspace is addressed with no filter
//                      (the caller must pick a member with -p or fan out with
//                      --workspace).
export std::expected<std::filesystem::path, std::string>
resolve_member_dir(const mcpp::manifest::Manifest& rootManifest,
                   const std::filesystem::path& rootDir,
                   std::string_view package_filter) {
    if (!rootManifest.workspace.present) return std::filesystem::path{};
    // "." and the rooted workspace's own package name select that package
    // (workspace design 2026-09-29 §7.1): it is a member of every command
    // that acts on the whole workspace, and `-p` names it like any other.
    if (!rootManifest.package.name.empty()
        && (package_filter == "." || package_filter == rootManifest.package.name
            || (!rootManifest.package.namespace_.empty()
                && package_filter == rootManifest.package.namespace_ + "."
                                     + rootManifest.package.name)))
        return rootDir;
    if (package_filter.empty()) {
        if (rootManifest.package.name.empty()) {
            return std::unexpected(std::string(
                "virtual workspace: specify -p <member> or --workspace"));
        }
        return std::filesystem::path{};  // rooted workspace, no filter → root
    }

    auto members = workspace_members(rootManifest, rootDir);

    // Step 1: the qualified name. Unique by construction (SPEC-001 §3.3 asks
    // a single index to keep `(namespace, name)` unique; two workspace
    // members sharing one is a manifest defect this resolver does not
    // adjudicate), so the first hit is taken without a warning.
    for (auto const& m : members)
        if (!m.namespace_.empty() && qualified_member_name(m) == package_filter)
            return m.dir;

    // Step 2: the bare package name, ignoring namespace.
    std::vector<std::size_t> byName;
    for (std::size_t i = 0; i < members.size(); ++i)
        if (!members[i].name.empty() && members[i].name == package_filter)
            byName.push_back(i);
    if (byName.size() > 1) {
        std::string names;
        for (auto i : byName)
            names += (names.empty() ? "" : ", ") + qualified_member_name(members[i]);
        return std::unexpected(std::format(
            "-p '{}' is ambiguous: it is the package name of {} members ({}). "
            "Write the qualified name (<namespace>.<name>) to select one.",
            package_filter, byName.size(), names));
    }

    // Step 3: the directory path or basename `[workspace] members` writes.
    // The path is compared as a path, so `./modules/base/`, and on Windows
    // `modules\base`, name the member written `modules/base`.
    auto as_path = [](std::string_view s) {
        auto p = std::filesystem::path(s).lexically_normal();
        return (!p.has_filename() && p.has_parent_path()) ? p.parent_path() : p;
    };
    const auto filterPath = as_path(package_filter);
    std::vector<std::size_t> byPath;
    for (std::size_t i = 0; i < members.size(); ++i) {
        auto basename = members[i].dir.filename().string();
        if (basename == package_filter || as_path(members[i].memberPath) == filterPath)
            byPath.push_back(i);
    }

    if (byName.size() == 1) {
        const auto& picked = members[byName.front()];
        if (!byPath.empty() && byPath.front() != byName.front()) {
            const auto& other = members[byPath.front()];
            mcpp::ui::warning(std::format(
                "-p '{}' is the package name of member '{}' ({}) and also the "
                "directory of member '{}' ({}); the package is selected. Write "
                "'{}' to select the other member.",
                package_filter, picked.memberPath, qualified_member_name(picked),
                other.memberPath, qualified_member_name(other), other.memberPath));
        }
        return picked.dir;
    }

    if (!byPath.empty()) {
        if (byPath.size() > 1) {
            std::string others;
            for (std::size_t k = 1; k < byPath.size(); ++k)
                others += std::format("{}'{}'", others.empty() ? "" : ", ",
                                      members[byPath[k]].memberPath);
            mcpp::ui::warning(std::format(
                "-p '{}' matches more than one member's directory; '{}' is "
                "selected (the first listed in [workspace] members). Also "
                "matched: {}.",
                package_filter, members[byPath.front()].memberPath, others));
        }
        return members[byPath.front()].dir;
    }

    std::string list;
    for (auto const& m : members) {
        auto label = m.name.empty() ? std::string("?")
                   : (m.namespace_.empty() ? m.name
                                            : std::format("{} ({})", m.name,
                                                          qualified_member_name(m)));
        list += std::format("\n  {} at '{}'", label, m.memberPath);
    }
    return std::unexpected(std::format(
        "workspace member '{}' not found. [workspace] members:{}",
        package_filter, list));
}


// ─── Workspace plans (workspace design 2026-09-29 §15) ──────────────────────
//
// A command on a workspace selects members and plans them under one root: the
// root package of a rooted workspace when it is selected, and otherwise a
// virtual root that holds the values one plan shares. The functions below are
// shared by the command layer, which groups the selected members into plans,
// and by the planner, which builds each plan's root; both read the same list
// of root-position values, so a group and its root cannot disagree about it.

// A selected member's effective manifest: loaded as a member, with the
// workspace's configuration inherited, exactly as the planner loads the member
// it builds.
export std::expected<mcpp::manifest::Manifest, std::string>
load_member_manifest(const mcpp::manifest::Manifest& workspace,
                     const std::filesystem::path& wsRoot,
                     std::string_view memberPath) {
    const auto dir = wsRoot / std::string(memberPath);
    if (memberPath == ".") {
        auto m = workspace;
        merge_workspace_deps(m, workspace, wsRoot);
        inherit_as_root_package(m, wsRoot);
        return m;
    }
    if (!std::filesystem::exists(dir / "mcpp.toml"))
        return std::unexpected(std::format(
            "workspace member '{}' has no mcpp.toml", memberPath));
    auto mm = mcpp::manifest::load(dir / "mcpp.toml", {.insideWorkspace = true});
    if (!mm) return std::unexpected(std::format(
        "workspace member '{}': {}", memberPath, mm.error().format()));
    // A relative `[indices].path` the member declares was written in the
    // member's directory. The plan's root is the workspace root, so the path
    // is anchored here, and two members that write the same relative path
    // for different trees have different root-position keys.
    for (auto& [_, idx] : mm->indices)
        if (idx.is_local() && idx.path.is_relative())
            idx.path = (dir / idx.path).lexically_normal();
    inherit_workspace_config(*mm, workspace, wsRoot);
    if (auto bad = workspace_inheritance_error(*mm, dir)) return std::unexpected(*bad);
    return std::move(*mm);
}

// The values of a member's manifest that are one value per plan: the
// toolchain request and the target rows, the C++ standard, the graph-wide
// `[build]` keys (among them `linkage`, which chooses the C runtime every
// object is compiled against), the profiles and the indices. Members whose
// keys are equal are planned together; the key is a canonical string of those
// values and nothing else, so a member's own flags, sources and dependencies
// never separate it from another member.
export std::string root_position_key(const mcpp::manifest::Manifest& m) {
    std::string s;
    auto field = [&](std::string_view name, std::string_view value) {
        s += name; s += '='; s += value; s += '\x1f';
    };
    auto list = [&](std::string_view name, const std::vector<std::string>& values) {
        s += name; s += '=';
        for (auto const& v : values) { s += v; s += '\x1e'; }
        s += '\x1f';
    };
    for (auto const& [platform, spec] : m.toolchain.byPlatform)
        field("toolchain." + platform, spec);
    for (auto const& [triple, e] : m.targetOverrides) {
        field("target." + triple + ".toolchain", e.toolchain);
        field("target." + triple + ".linkage", e.linkage);
        field("target." + triple + ".cxx_runtime", e.cxxRuntime);
        field("target." + triple + ".sysroot", e.sysroot);
        field("target." + triple + ".min_api", std::to_string(e.minApiLevel));
        list("target." + triple + ".runner", e.runner);
        for (auto const& [name, argv] : e.namedRunners)
            list("target." + triple + ".runners." + name, argv);
    }
    field("standard", m.package.standard);
    auto const& b = m.buildConfig;
    list("dialect_cxxflags", b.dialectCxxflags);
    field("cxx_runtime", b.cxxRuntime);
    field("cxx_runtime_tests", b.cxxRuntimeTests);
    field("cxx_runtime_shared", b.cxxRuntimeShared);
    field("static_stdlib", b.staticStdlib ? "1" : "0");
    field("linkage", b.linkage);
    field("dependency_linkage", b.dependencyLinkage);
    field("target", b.target);
    field("macos_deployment_target", b.macosDeploymentTarget);
    field("ios_deployment_target", b.iosDeploymentTarget);
    field("abi_threads", b.abiThreadsDeclared ? (b.abiThreads ? "1" : "0") : "-");
    field("abi_exceptions", b.abiExceptionsDeclared ? (b.abiExceptions ? "1" : "0") : "-");
    field("accel", b.accel);
    field("bmi_schedule", b.bmiSchedule);
    field("jobs", b.jobs);
    field("cache", b.cacheMode);
    field("allow_host_libs", b.allowHostLibs ? "1" : "0");
    field("platform_dependencies", b.platformDependencies);
    field("default_profile", b.defaultProfile);
    list("runner", b.runner);
    for (auto const& [name, r] : b.namedRunners)
        list("runners." + name + (r.longLived ? ".long" : ""), r.argv);
    field("run_exclusive", b.runExclusive ? "1" : "0");
    for (auto const& [name, pr] : m.profiles) {
        field("profile." + name, std::format("{}/{}/{}/{}/{}", pr.optLevel,
              pr.debug ? 1 : 0, pr.lto ? 1 : 0, pr.strip ? 1 : 0,
              pr.dependencyLinkageDeclared ? pr.dependencyLinkage : std::string("-")));
        list("profile." + name + ".cflags", pr.cflags);
        list("profile." + name + ".cxxflags", pr.cxxflags);
        list("profile." + name + ".ldflags", pr.ldflags);
    }
    for (auto const& [name, idx] : m.indices)
        field("index." + name, std::format("{}#{}#{}#{}#{}", idx.url, idx.rev, idx.tag,
                                            idx.branch, idx.path.generic_string()));
    for (auto const& [cap, pin] : m.capabilityPins) field("cap." + cap, pin);
    for (auto const& [tool, pin] : m.toolOverrides) field("tool." + tool, pin);
    return s;
}

// The root of a plan whose selected members do not include a rooted
// workspace's own package. It carries the root-position values of the first
// selected member, which every member of the plan shares (`root_position_key`),
// the workspace's own tables, and nothing a package owns: no sources, targets,
// dependencies, flags, hooks or build program. Its dependencies are the member
// edges the planner adds.
export mcpp::manifest::Manifest
virtual_workspace_root(const mcpp::manifest::Manifest& workspace,
                       const mcpp::manifest::Manifest& first,
                       const std::filesystem::path& wsRoot) {
    mcpp::manifest::Manifest v;
    v.sourcePath = wsRoot / "mcpp.toml";
    v.package.name = "workspace";
    v.package.version = "0.0.0";
    v.package.virtualRoot = true;
    v.package.standard = first.package.standard;
    v.package.standardDeclared = first.package.standardDeclared;
    v.language.standard = first.language.standard;
    v.cppStandard = first.cppStandard;
    v.toolchain = first.toolchain;
    v.targetOverrides = first.targetOverrides;
    v.indices = first.indices;
    v.profiles = first.profiles;
    v.capabilityPins = first.capabilityPins;
    v.toolOverrides = first.toolOverrides;
    v.workspace = workspace.workspace;
    v.xlings.subos = workspace.xlings.subos;
    v.xlings.subosDeclared = workspace.xlings.subosDeclared;
    auto& b = v.buildConfig;
    auto const& f = first.buildConfig;
    b.sourcesDeclared = true;
    b.runner = f.runner;
    b.namedRunners = f.namedRunners;
    b.runExclusive = f.runExclusive;
    b.jobs = f.jobs;
    b.bmiSchedule = f.bmiSchedule;
    b.accel = f.accel;
    b.staticStdlib = f.staticStdlib;
    b.cxxRuntime = f.cxxRuntime;
    b.cxxRuntimeTests = f.cxxRuntimeTests;
    b.cxxRuntimeShared = f.cxxRuntimeShared;
    b.target = f.target;
    b.dialectCxxflags = f.dialectCxxflags;
    b.abiThreads = f.abiThreads;
    b.abiThreadsDeclared = f.abiThreadsDeclared;
    b.abiExceptions = f.abiExceptions;
    b.abiExceptionsDeclared = f.abiExceptionsDeclared;
    b.allowHostLibs = f.allowHostLibs;
    b.macosDeploymentTarget = f.macosDeploymentTarget;
    b.iosDeploymentTarget = f.iosDeploymentTarget;
    b.defaultProfile = f.defaultProfile;
    b.linkage = f.linkage;
    b.dependencyLinkage = f.dependencyLinkage;
    b.cacheMode = f.cacheMode;
    b.platformDependencies = f.platformDependencies;
    return v;
}

// The directory a member's products are placed in, below the configuration's
// `bin/` (workspace design 2026-09-29 §5.2): the member's package name, or
// `<namespace>.<name>` when another member of the workspace has the same
// name. The workspace's own package, `"."`, keeps `bin/` itself (empty).
// Decided over every member of the workspace, not over one selection, so a
// member's directory is the same whichever command built it.
export std::string product_directory_name(const std::vector<WorkspaceMember>& all,
                                          std::string_view memberPath) {
    if (memberPath == ".") return {};
    auto it = std::ranges::find_if(all, [&](const WorkspaceMember& m) {
        return m.memberPath == memberPath;
    });
    if (it == all.end() || it->name.empty()) {
        std::string s(memberPath);
        for (auto& c : s) if (c == '/' || c == '\\') c = '.';
        return s;
    }
    // Compared without case: a Windows or macOS file system gives two names
    // that differ only in case one directory.
    auto folded = [](std::string v) {
        for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    const auto shared = std::ranges::count_if(all, [&](const WorkspaceMember& m) {
        return folded(m.name) == folded(it->name);
    });
    return shared > 1 ? qualified_member_name(*it) : it->name;
}

} // namespace mcpp::project
