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
export void inherit_workspace_config(mcpp::manifest::Manifest& member,
                                     const mcpp::manifest::Manifest& workspace,
                                     const std::filesystem::path& wsRoot) {
    merge_workspace_deps(member, workspace, wsRoot);

    if (member.toolchain.byPlatform.empty())
        member.toolchain = workspace.toolchain;
    for (auto& [triple, entry] : workspace.targetOverrides)
        if (!member.targetOverrides.contains(triple))
            member.targetOverrides[triple] = entry;
    inherit_workspace_indices(member, workspace, wsRoot);

    const auto& inh = workspace.workspace.inherited;

    // `[workspace.package]`. The standard is the load-bearing one: a C++ module
    // graph has ONE standard, so a workspace that states it once is how a
    // monorepo stops depending on every member remembering to.
    //
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
        // default out of the normalised copy. Same class of defect as a
        // recorded field with no reader, one struct over.
        if (auto cfg = mcpp::manifest::normalize_cpp_standard(inh.standard))
            member.cppStandard = *cfg;
    }
    if (member.package.version.empty())     member.package.version     = inh.version;
    // (the "still missing after inheritance" refusal is in
    //  `workspace_inheritance_error` below — one predicate, both call sites)
    if (member.package.license.empty())     member.package.license     = inh.license;
    if (member.package.description.empty()) member.package.description = inh.description;
    if (member.package.repo.empty())        member.package.repo        = inh.repo;
    if (member.package.authors.empty())     member.package.authors     = inh.authors;

    // `[workspace.build]`. Vectors append workspace-FIRST so a member's own
    // flag lands later on the command line, where the compiler lets it win.
    if (inh.buildPresent) {
        auto& b = member.buildConfig;
        const auto& w = inh.build;
        auto prepend = [](auto& dst, const auto& src) {
            if (src.empty()) return;
            dst.insert(dst.begin(), src.begin(), src.end());
        };
        prepend(b.cflags,   w.cflags);
        prepend(b.cxxflags, w.cxxflags);
        prepend(b.ldflags,  w.ldflags);
        prepend(b.defines,  w.defines);
        prepend(b.dialectCxxflags, w.dialectCxxflags);
        prepend(b.includeDirs,        w.includeDirs);
        prepend(b.includeDirsAfter,   w.includeDirsAfter);
        prepend(b.privateIncludeDirs, w.privateIncludeDirs);
        if (b.cStandard.empty())            b.cStandard            = w.cStandard;
        if (b.linkage.empty())              b.linkage              = w.linkage;
        if (b.target.empty())               b.target               = w.target;
        if (b.cxxRuntime.empty())           b.cxxRuntime           = w.cxxRuntime;
        if (b.dependencyLinkage.empty())    b.dependencyLinkage    = w.dependencyLinkage;
        if (b.macosDeploymentTarget.empty())
            b.macosDeploymentTarget = w.macosDeploymentTarget;
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

// Resolve which member directory a workspace command acts on, for the
// single-member case. Shares the match rule (basename OR member path) with
// prepare_build's member switch, so `build -p X` and `test -p X` agree.
// Returns:
//   - the member dir   when `package_filter` names a member,
//   - empty path       when no switch applies (not a workspace, or a rooted
//                      workspace with no filter → act on the root package),
//   - error            when the filter names an unknown member, or a *virtual*
//                      workspace is addressed with no filter (the caller must
//                      pick a member with -p or fan out with --workspace).
export std::expected<std::filesystem::path, std::string>
resolve_member_dir(const mcpp::manifest::Manifest& rootManifest,
                   const std::filesystem::path& rootDir,
                   std::string_view package_filter) {
    if (!rootManifest.workspace.present) return std::filesystem::path{};
    if (!package_filter.empty()) {
        for (auto& mp : rootManifest.workspace.members) {
            auto basename = std::filesystem::path(mp).filename().string();
            if (basename == package_filter || mp == package_filter)
                return rootDir / mp;
        }
        return std::unexpected(std::format(
            "workspace member '{}' not found in [workspace].members", package_filter));
    }
    if (rootManifest.package.name.empty()) {
        return std::unexpected(std::string(
            "virtual workspace: specify -p <member> or --workspace"));
    }
    return std::filesystem::path{};  // rooted workspace, no filter → root package
}

} // namespace mcpp::project
