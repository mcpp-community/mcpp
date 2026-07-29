// mcpp.pm.index_route — which index answers for a namespace, how its
// descriptors are read, and when a miss is allowed to mean anything.
//
// Three transports serve xpkg descriptors, and picking the wrong one is
// indistinguishable from "this package does not exist":
//
//   [indices] <ns> = { path = "…" }  → read straight off the filesystem
//   [indices] <ns> = { url  = "…" }  → a project-level clone under <root>/.mcpp
//   builtin / default namespace      → the global registry in <XLINGS_HOME>/data
//
// `mcpp.build.prepare` has always dispatched on that rule while resolving
// dependencies. `mcpp add` grew a second, narrower existence check that only
// ever consulted the global registry (#305/#307), so a package served by a
// project index read as missing and a perfectly valid dependency was refused.
// Both callers now route through this module: what `mcpp add` accepts and what
// `mcpp build` can resolve cannot drift apart again.
//
// The other half of the contract is `authoritative_for`. Refusing to write a
// dependency is only correct when the index that would serve it is readable
// and did not serve it. A custom git index is cloned lazily during install, so
// while it is still absent it can neither serve nor refute anything; and a
// foreign namespace is outside the identity rules mcpp enforces at all (xim's
// descriptors declare no `namespace` field, so `(xim, nasm)` can never match
// the identity gate no matter how present the index is). In both cases a miss
// is inconclusive and callers must fall through rather than hard-fail —
// exactly the "keep the historical fall-through" rule prepare applies to lazy
// git indices today.

export module mcpp.pm.index_route;

import std;
import mcpp.config;
import mcpp.fetcher;
import mcpp.manifest;
import mcpp.pm.dep_spec;
import mcpp.pm.index_spec;
import mcpp.project;

export namespace mcpp::pm {

using IndexMap = std::map<std::string, IndexSpec>;

// A candidate coordinate together with the descriptor that proved it.
struct DescriptorHit {
    DependencyCoordinate coord;       // the candidate that matched
    std::string          declaredNs;  // descriptor's declared namespace ("" = none)
    std::string          lua;         // descriptor source
};

struct IndexRoute {
    const IndexMap*                   indices     = nullptr;  // manifest [indices]
    std::filesystem::path             projectRoot;
    const mcpp::config::GlobalConfig* cfg         = nullptr;   // null → global registry unreadable

    // The [indices] entry serving `ns`, or nullptr for the builtin registry.
    const IndexSpec* find_for_ns(std::string_view ns) const;
    // A custom git index, not yet cloned: readable only after install runs.
    bool lazy_git(std::string_view ns) const;
    // Can a miss in this namespace be taken as proof of absence?
    bool authoritative_for(std::string_view ns) const;
    // Read the descriptor for one coordinate through the right transport.
    std::optional<std::string> read(const DependencyCoordinate& coord) const;
};

struct Lookup {
    std::optional<DescriptorHit> hit;
    // false → at least one candidate lives behind an index that cannot refute
    // it, so "no hit" says nothing and the caller must not hard-fail.
    bool conclusive = true;
};

// Walk ordered candidates and return the first descriptor that DECLARES the
// requested identity, applying the same two rules prepare's dependency
// disambiguation applies.
Lookup lookup_descriptor(const IndexRoute& route,
                         const std::vector<DependencyCoordinate>& candidates);

// Diagnostic only: fully-qualified names carrying `shortName` under some other
// namespace, for did-you-mean text on an already-failed lookup.
std::vector<std::string> cross_namespace_matches(const IndexRoute& route,
                                                 std::string_view shortName);

// The `[indices]` a project at `root` effectively sees, including the
// workspace-root inheritance a member gets for free (#224).
IndexMap effective_indices(const std::filesystem::path& root);

} // namespace mcpp::pm

namespace mcpp::pm {

const IndexSpec* IndexRoute::find_for_ns(std::string_view ns) const {
    if (!indices) return nullptr;
    if (ns.empty() || ns == kDefaultNamespace) {
        // R6: `[indices] default = {...}` (normalized to kDefaultNamespace by
        // toml.cppm) redirects the default namespace — return it when present
        // instead of unconditionally falling back to the builtin index.
        auto it = indices->find(std::string(kDefaultNamespace));
        return it == indices->end() ? nullptr : &it->second;
    }
    if (auto it = indices->find(std::string(ns)); it != indices->end()) {
        return &it->second;
    }
    auto root = ns.substr(0, ns.find('.'));
    for (auto& [idxName, spec] : *indices) {
        if (idxName == ns) return &spec;
        if (idxName == root) return &spec;
    }
    return nullptr;
}

bool IndexRoute::lazy_git(std::string_view ns) const {
    auto* idx = find_for_ns(ns);
    return idx && !idx->is_builtin() && !idx->is_local();
}

bool IndexRoute::authoritative_for(std::string_view ns) const {
    if (auto* idx = find_for_ns(ns)) {
        // Declared index: readable now (local path / builtin registry) or
        // cloned later (git), in which case it proves nothing yet.
        return idx->is_local() || idx->is_builtin();
    }
    if (!cfg) return false;   // global registry not even loaded
    // No [indices] entry, so the builtin registry answers. It is authoritative
    // for the namespaces whose identity mcpp itself defines — the default
    // namespace, its nested children, `compat`, and the namespace-less
    // discovery rung. A third-party namespace that merely happens to be filed
    // in some index is not something a miss here can refute.
    if (ns.empty()) return true;
    if (ns == kDefaultNamespace || ns == kCompatNamespace) return true;
    return ns.starts_with(std::string(kDefaultNamespace) + ".");
}

std::optional<std::string>
IndexRoute::read(const DependencyCoordinate& coord) const {
    auto* idx = find_for_ns(coord.namespace_);
    if (idx && idx->is_local()) {
        return mcpp::fetcher::Fetcher::read_xpkg_lua_from_path(
            mcpp::config::resolve_project_index_path(projectRoot, *idx),
            coord.namespace_, coord.shortName);
    }
    if (idx && !idx->is_builtin()) {
        return mcpp::fetcher::Fetcher::read_xpkg_lua_from_project_data(
            projectRoot, coord.namespace_, coord.shortName);
    }
    if (!cfg) return std::nullopt;
    return mcpp::fetcher::Fetcher(*cfg).read_xpkg_lua(
        coord.namespace_, coord.shortName);
}

Lookup lookup_descriptor(const IndexRoute& route,
                         const std::vector<DependencyCoordinate>& candidates) {
    Lookup out;
    for (auto& candidate : candidates) {
        if (!route.authoritative_for(candidate.namespace_)) out.conclusive = false;

        auto lua = route.read(candidate);
        if (!lua) continue;
        // The descriptor must DECLARE the requested identity. `allowLegacy-
        // BareDefault=false` matches prepare's disambiguation: a descriptor
        // with no namespace is reached through the explicit `(∅, name)` rung
        // below, not by being waved through on the default-namespace rung.
        if (!mcpp::manifest::xpkg_lua_identity_matches(
                *lua, candidate.namespace_, candidate.shortName,
                /*allowLegacyBareDefault=*/false)) {
            continue;
        }
        auto declaredNs = mcpp::manifest::extract_xpkg_namespace(*lua);
        // INV-RESOLVE (#278) — `(∅, name)` is the "upstream package that
        // declares no namespace" rung, NOT a cross-namespace wildcard. The
        // identity gate is deliberately permissive there (`mcpp new --template
        // X` discovers by short name alone), so the narrowing lives here.
        if (candidate.namespace_.empty() && !declaredNs.empty()) continue;

        out.hit = DescriptorHit{ candidate, std::move(declaredNs), std::move(*lua) };
        break;
    }
    return out;
}

std::vector<std::string> cross_namespace_matches(const IndexRoute& route,
                                                 std::string_view shortName) {
    if (!route.cfg) return {};
    mcpp::fetcher::Fetcher fetcher(*route.cfg);
    auto roots = fetcher.builtin_index_roots();
    if (route.indices) {
        for (auto& [_, spec] : *route.indices) {
            if (spec.is_local()) {
                roots.push_back(mcpp::config::resolve_project_index_path(
                    route.projectRoot, spec));
            }
        }
    }
    return mcpp::fetcher::Fetcher::scan_fqns_with_short_name(roots, shortName);
}

IndexMap effective_indices(const std::filesystem::path& root) {
    auto m = mcpp::manifest::load(root / "mcpp.toml");
    if (!m) return {};
    if (!m->indices.empty()) return m->indices;

    // A member with no [indices] of its own inherits the workspace root's,
    // whose relative `path` is anchored at the ROOT, not the member (#224).
    auto wsRoot = mcpp::project::find_workspace_root(root);
    if (wsRoot.empty()) return m->indices;
    auto ws = mcpp::manifest::load(wsRoot / "mcpp.toml");
    if (!ws || !ws->workspace.present) return m->indices;
    mcpp::project::inherit_workspace_indices(*m, *ws, wsRoot);
    return m->indices;
}

} // namespace mcpp::pm
