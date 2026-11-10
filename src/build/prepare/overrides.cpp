// overrides.cpp -- P4b: the root's overrides (design 2026-11-10 supplement,
// T2). A declaration written `override = true` in the root's dependency tables,
// or in `[workspace.dependencies]`, answers every request for its package in the
// graph its table governs, whatever version the request names: the requests a
// merge cannot reconcile are settled by the project, not by the order of the
// walk. Split from graph.cpp, whose steps call these.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.diag;
import mcpp.ui;
import mcpp.manifest;
import mcpp.pm.compat;
import mcpp.pm.resolver;   // is_version_constraint: a literal version is exact
import mcpp.version_req;

namespace mcpp::build {

namespace {

// How a declaration names its source, for the line that reports a replaced
// request: a version constraint, a path, or a repository and its reference.
std::string source_of(const mcpp::manifest::DependencySpec& s) {
    if (s.isPath()) return std::format("path {}", s.path);
    if (s.isGit()) return std::format("{} {}={}", s.git, s.gitRefKind, s.gitRev);
    return s.version.empty() ? std::string("*") : s.version;
}

// Whether a version request admits the override's version: then the override
// changes nothing about it, and no line is printed for it.
bool admits(const mcpp::manifest::DependencySpec& asked, const mcpp::manifest::DependencySpec& o) {
    if (asked.isPath() || asked.isGit() || o.isPath() || o.isGit()) return false;
    std::string_view v = o.version;
    if (v.starts_with('=')) v.remove_prefix(1);
    // A literal version is exact, as the resolver reads it (`is_version_constraint`).
    if (!asked.version.empty() && !mcpp::pm::is_version_constraint(asked.version)) {
        std::string_view a = asked.version;
        if (a.starts_with('=')) a.remove_prefix(1);
        return a == v;
    }
    auto version = mcpp::version_req::parse_version(v);
    auto req = mcpp::version_req::parse_req(asked.version.empty() ? std::string("*") : asked.version);
    return version && req && mcpp::version_req::matches(*req, *version);
}

} // namespace

void step4b_read_root_overrides(PrepareState& state) {
    state.rootOverrides.clear();
    if (!state.m) return;
    const auto& root = *state.m;
    auto add = [&](const std::map<std::string, mcpp::manifest::DependencySpec>& table,
                   std::string_view tableName, std::string_view owner, bool buildSide) {
        for (auto const& [key, spec] : table) {
            if (!spec.override_) continue;
            PrepareState::RootOverride o;
            o.key = key;
            o.spec = spec;
            o.ns = spec.namespace_;
            o.shortName = spec.shortName.empty() ? key : spec.shortName;
            bool legacy = spec.legacyDottedKey;
            mcpp::pm::compat::normalize_nested_namespace(o.ns, o.shortName, legacy);
            o.declaredIn = std::format("[{}] of {}", tableName, owner);
            o.buildSide = buildSide;
            state.rootOverrides.push_back(std::move(o));
        }
    };
    add(root.dependencies, "dependencies", root.package.name, false);
    if (state.includeDevDeps) add(root.devDependencies, "dev-dependencies", root.package.name, false);
    add(root.buildDependencies, "build-dependencies", root.package.name, true);
    // The workspace's: they govern every plan of its members, and declaring one
    // there does not make a member depend on the package.
    if (state.wsManifest && state.wsManifest->workspace.present)
        add(state.wsManifest->workspace.dependencies, "workspace.dependencies", "the workspace", false);
}

bool step4b_apply_root_override(PrepareState& state, WorkItem& item) {
    if (state.declaredByRoot(item.consumerDepIndex)) return false;
    if (item.spec.override_) {
        // An override is the project's decision; a library that writes one
        // would decide for every project that uses it.
        const auto line = std::format("{}: `override` on '{}' is ignored: only the root's dependency tables "
                                      "and [workspace.dependencies] override", item.requestedBy, item.name);
        if (state.overrideStated.insert(line).second) mcpp::diag::warning("dependency/override", line);
        item.spec.override_ = false;
    }
    const auto& asked = item.spec;
    const std::string askedShort = asked.shortName.empty() ? item.name : asked.shortName;
    for (auto const& o : state.rootOverrides) {
        if (o.buildSide != item.buildOnly) continue;
        if (o.shortName != askedShort) continue;
        if (!asked.namespace_.empty() && !o.ns.empty() && asked.namespace_ != o.ns) continue;
        const bool quiet = admits(asked, o.spec)
                        || (asked.isPath() == o.spec.isPath() && asked.isGit() == o.spec.isGit()
                            && source_of(asked) == source_of(o.spec));
        if (!quiet) {
            const auto line = std::format("{} {} (requested by {}) -> {} ({})",
                                          o.ns.empty() ? o.shortName : o.ns + "." + o.shortName,
                                          source_of(asked), item.requestedBy, source_of(o.spec), o.declaredIn);
            if (state.overrideStated.insert(line).second) mcpp::ui::status("Overrode", line);
        }
        auto& s = item.spec;
        s.version = o.spec.version;
        s.path = o.spec.path;
        s.git = o.spec.git;
        s.gitRev = o.spec.gitRev;
        s.gitRefKind = o.spec.gitRefKind;
        s.patches = o.spec.patches;
        s.patchesDigest.clear();
        if (!o.ns.empty()) s.namespace_ = o.ns;
        s.shortName = o.shortName;
        // A path is resolved where the root wrote it.
        if (o.spec.isPath()) item.resolveRoot.clear();
        item.originalConstraint = o.spec.version;
        item.overridden = true;
        return true;
    }
    return false;
}

bool step4b_root_overrides_group(PrepareState& state, const WorkItem& item, std::string_view shortName,
                                 std::string_view groupVersion) {
    if (item.overridden) return true;
    if (!state.declaredByRoot(item.consumerDepIndex) || !item.spec.override_) return false;
    const auto line = std::format("{} {} overrides the version {} names, {}: the platform gives the "
                                  "default, and this project chose another",
                                  shortName, source_of(item.spec), state.platformGroupFrom, groupVersion);
    if (state.overrideStated.insert(line).second) mcpp::diag::warning("dependency/override-group", line);
    return true;
}

void step4b_note_exact_pin(PrepareState& state, const WorkItem& item, const mcpp::manifest::Manifest& dep) {
    if (item.overridden) return;
    std::string_view v = item.originalConstraint;
    if (v.starts_with('=')) v.remove_prefix(1);
    if (v.empty() || !mcpp::version_req::parse_version(v)) return;   // a range, not a pin
    const bool layer = std::ranges::any_of(dep.provides, [](const std::string& c) { return c.starts_with("mcpp:"); });
    const bool abi = !dep.kernelAbiProvidesInterfaces.empty();
    if (!layer && !abi) return;
    // A member of the platform's version group is pinned by the family that
    // releases it together; the group, not the pin, decides there.
    const auto depShort = item.spec.shortName.empty() ? item.name : item.spec.shortName;
    if (state.platformGroup.contains(depShort)) return;
    const auto what = layer ? "provides a target-side layer" : "provides a kernel ABI";
    if (state.declaredByRoot(item.consumerDepIndex)) {
        state.sharedExactPins.push_back(std::format("{} = \"{}\" ({} {}; a range is \"^{}\")",
                                                    item.name, item.originalConstraint, dep.package.name, what, v));
        return;
    }
    const auto line = std::format(
        "{} pins {} = \"{}\": {} {}, so every project that uses {} is held to that release. "
        "A range (\"^{}\") lets the project choose; a project chooses for the whole graph with "
        "`override = true`", item.requestedBy, item.name, item.originalConstraint, dep.package.name, what,
        item.requestedBy, v);
    if (state.overrideStated.insert(line).second) mcpp::diag::note("dependency/exact-pin", line);
}

} // namespace mcpp::build
