// graph_requests.cpp -- P4b: what a request of the dependency worklist is bound
// by, which side of the build makes it, and which resolution it is of: the
// target's or a build program's (design 2026-11-19 §21 A1). Split from
// graph.cpp at the boundary of a request's own facts, when a request learned to
// carry the platform's version group into its merges and a package's tables
// came to be read by the row each of its instances is built for (mcpp#956;
// SPEC-013 §7.5, SPEC-004 §4.8); the resolutions' helpers followed, when they
// took graph.cpp past its limit.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.build.prepare_inputs;
import mcpp.manifest;
import mcpp.pm.dep_spec;
import mcpp.pm.resolver;
import mcpp.toolchain.triple;
import mcpp.version_req;

namespace mcpp::build {

// What a request is bound by: the range as written and, where the platform's
// version group binds it, the group's version (SPEC-013 §7.5).
std::string request_constraint(const WorkItem& item) {
    std::string out = item.originalConstraint.empty() ? std::string("*") : item.originalConstraint;
    if (!item.groupConstraint.empty()) out += "," + item.groupConstraint;
    return out;
}

// The requests a record answers, each with its requester: the written range
// under who wrote it, the group's version under the platform that states it.
void note_request(const PrepareState& state, ResolvedRecord& r, const WorkItem& item) {
    const std::string written = item.originalConstraint.empty() ? std::string("*") : item.originalConstraint;
    r.constraint += (r.constraint.empty() ? "" : ",") + written;
    r.constraintsBy.emplace_back(item.requestedBy, written);
    if (item.groupConstraint.empty()) return;
    const auto by = std::format("the version group of {}", state.platformGroupFrom);
    for (auto const& [who, c] : r.constraintsBy)
        if (who == by && c == item.groupConstraint) return;
    r.constraint += "," + item.groupConstraint;
    r.constraintsBy.emplace_back(by, item.groupConstraint);
}

// Every request of a record still in the graph, for a refusal: who asked and
// for what, one per line.
std::string requests_of(const PrepareState& state, const ResolvedRecord& r) {
    std::string out;
    for (auto const& [by, c] : r.constraintsBy) {
        if (state.supersededRequesters.contains(by)) continue;
        out += std::format("\n      '{}' by {}", c, by);
    }
    return out;
}

// A PACKAGE'S DEPENDENCY TABLE IS READ BY THE ROW IT IS BUILT FOR (SPEC-004
// §4.8; mcpp#956). A package reached only through build dependencies is built
// for this machine --- it is a module of a build program --- and its
// `[target.<selector>.dependencies]` were read by the build's target: crossing
// to macOS, a library of a build program asked for the macOS implementation of
// its kernel layer, which no program of the build links, while the Linux one
// its build program does link was resolved outside the walk and absent from
// the lock. A package both sides ask for is built for each, and each side
// reads the table by its own row.

// The row the build programs are built for: this machine's, or the target the
// host graph's platform serves on it (SPEC-013 §6).
static std::expected<cfgpred::Ctx, std::string> host_row(PrepareState& state) {
    if (!state.hostPlatformSel.none) {
        if (auto r = load_host_platform(state); !r) return std::unexpected(r.error());
        if (!state.hostPlatformTarget.empty())
            return cfgpred::context_for(state.hostPlatformTarget,
                                        state.hostPlatformSel.ref.spec.shortName);
    }
    return cfgpred::context_for(mcpp::toolchain::triple::host_triple().str());
}

// One of a package's dependency tables as the host row reads it. The stored
// table is the target's reading: the tables as written, then each conditional
// table whose selector the target matches. The host's reading differs by the
// conditional tables only one of the two rows matches. A selector that names a
// layer is left as the target read it: the host row's layers are chosen when
// the build programs' own graph is prepared, which reads those itself.
static DependencyTable
host_row_table(const mcpp::manifest::Manifest& dm, const cfgpred::Ctx& target, const cfgpred::Ctx& host,
               DependencyTable mcpp::manifest::Manifest::* table,
               DependencyTable mcpp::manifest::ConditionalConfig::* conditional) {
    auto out = dm.*table;
    if (!dm.beforeConditionalMerge) return out;
    auto const& written = (*dm.beforeConditionalMerge).*table;
    for (auto const& cc : dm.conditionalConfigs) {
        if (cfgpred::uses_layer(cc.predicate)) continue;
        if (!cfgpred::matches(cc.predicate, target) || cfgpred::matches(cc.predicate, host)) continue;
        for (auto const& [key, spec] : cc.*conditional) {
            std::erase_if(out, [&](auto const& entry) {
                return entry.first == key || same_dependency_identity(entry.second, spec);
            });
            if (auto was = written.find(key); was != written.end()) out[key] = was->second;
        }
    }
    for (auto const& cc : dm.conditionalConfigs) {
        if (cfgpred::uses_layer(cc.predicate)) continue;
        if (cfgpred::matches(cc.predicate, target) || !cfgpred::matches(cc.predicate, host)) continue;
        replace_dependencies(out, cc.*conditional);
    }
    return out;
}

// The requests one side makes of a package's dependencies: the tables as that
// side's row reads them, and for each entry whether the other side's row has
// it. An entry of this machine's reading alone is a request of the package's
// build-program instance, so its edge is a build-table edge; an entry of the
// target's reading alone is marked so that the build program does not take it.
std::expected<SideRequests, std::string>
side_requests(PrepareState& state, const mcpp::manifest::Manifest& dm, bool hostSide) {
    SideRequests out{ dm.dependencies, dm.buildDependencies, {}, {} };
    if (dm.conditionalConfigs.empty() || !dm.beforeConditionalMerge) return out;
    auto host = host_row(state);
    if (!host) return std::unexpected(host.error());
    const auto target = state.cfgCtx();
    if (host->triple == target.triple && host->platformPackage == target.platformPackage) return out;
    const auto hostDependencies = host_row_table(dm, target, *host, &mcpp::manifest::Manifest::dependencies,
                                                 &mcpp::manifest::ConditionalConfig::dependencies);
    for (auto const& [key, _] : hostDependencies)
        if (!dm.dependencies.contains(key)) out.ofThisMachineAlone.insert(key);
    for (auto const& [key, _] : dm.dependencies)
        if (!hostDependencies.contains(key)) out.ofTheTargetAlone.insert(key);
    if (hostSide) {
        out.dependencies = hostDependencies;
        out.buildDependencies = host_row_table(dm, target, *host, &mcpp::manifest::Manifest::buildDependencies,
                                               &mcpp::manifest::ConditionalConfig::buildDependencies);
    }
    return out;
}

// One request of a side, as a work item.
WorkItem side_item(const SideRequests& side, const std::string& childName,
                          mcpp::manifest::DependencySpec childReq, const std::string& label,
                          std::size_t consumer, const std::filesystem::path& root, const WorkItem& item) {
    const bool ofThisMachine = side.ofThisMachineAlone.contains(childName);
    WorkItem out{childName, childReq, label, childReq.version, consumer, root, item.devOnly,
                 item.buildOnly || ofThisMachine, /*fromBuildTable=*/ofThisMachine};
    out.targetRowOnly = !item.buildOnly && side.ofTheTargetAlone.contains(childName);
    // A request of this machine's reading alone serves the consumer's build
    // program; every other request is of the program its consumer's was for.
    out.scope = item.scope.empty() && ofThisMachine ? program_scope(label) : item.scope;
    return out;
}

std::string program_scope(std::string_view label) {
    return std::format("build:{}", label);
}

std::string root_program_scope(const PrepareState& state) {
    auto const& pkg = state.m->package;
    const bool qualified = pkg.namespace_.empty() || pkg.name.starts_with(pkg.namespace_ + ".");
    return std::format("build:{}{}{}@{}", qualified ? "" : pkg.namespace_, qualified ? "" : ".",
                       pkg.name, pkg.version);
}

std::string scope_display(std::string_view scope) {
    if (scope.empty()) return "a build program";
    if (scope.starts_with("build:")) scope.remove_prefix(6);
    return std::format("the build program of {}", scope);
}

// A record of `identity` (its scope ignored): the target's first, else the
// first program's; null when no resolution holds the package.
const ResolvedRecord* any_record_of(const PrepareState& state, const ResolvedKey& identity) {
    auto target = identity;
    target.scope.clear();
    if (auto it = state.resolved.find(target); it != state.resolved.end()) return &it->second;
    for (auto const& [k, r] : state.resolved)
        if (k.ns == identity.ns && k.shortName == identity.shortName && k.variant == identity.variant)
            return &r;
    return nullptr;
}

// Whether `version` answers `constraint` as a request does: a constraint by
// its comparators, a bare version as itself.
static bool request_admits(std::string_view constraint, std::string_view version) {
    if (constraint.empty() || constraint == "*") return true;
    if (!mcpp::pm::is_version_constraint(std::string(constraint))) return constraint == version;
    auto req = mcpp::version_req::parse_req(constraint);
    auto ver = mcpp::version_req::parse_version(version);
    return req && ver && mcpp::version_req::matches(*req, *ver);
}

// THE VERSION A PROGRAM'S REQUEST TAKES FROM THE BUILD (§21 A1): the newest
// version of the package that the target or another program resolved and the
// request admits, when the program has no record of the package yet.
std::optional<std::string> held_version_for(const PrepareState& state, const WorkItem& item,
                                            const mcpp::manifest::DependencySpec& spec,
                                            const std::string& name) {
    if (!spec.patches.empty()) return std::nullopt;
    const std::string shortName = spec.shortName.empty() ? name : spec.shortName;
    if (state.resolved.contains(ResolvedKey{spec.namespace_, shortName, {}, item.scope})) return std::nullopt;
    const auto constraint = request_constraint(item);
    std::optional<std::string> best;
    std::optional<mcpp::version_req::Version> bestVersion;
    for (auto const& [k, r] : state.resolved) {
        if (k.scope == item.scope || k.ns != spec.namespace_ || k.shortName != shortName
            || !k.variant.empty() || r.source != "version" || r.version.empty())
            continue;
        if (!request_admits(constraint, r.version)) continue;
        auto v = mcpp::version_req::parse_version(r.version);
        if (!v) continue;
        if (!bestVersion || *bestVersion < *v) { bestVersion = *v; best = r.version; }
    }
    return best;
}

// Whether a slot serves a resolution other than `key`'s program: another
// program's record or the target's points at it, or an edge another made.
bool slot_held_elsewhere(const PrepareState& state, std::size_t depIndex, const ResolvedKey& key) {
    for (auto const& [k, r] : state.resolved)
        if (r.depIndex == depIndex && k.scope != key.scope) return true;
    return std::ranges::any_of(state.dependencyEdges, [&](const DependencyEdge& e) {
        return e.dependencyPackageIndex == depIndex + 1 && e.scope != key.scope;
    });
}

// A program's record resolved again after a move (§21 A1): the edges the
// program alone made to the slot it left now reach the new one, and the record
// keeps every request it answered.
void finish_scope_move(PrepareState& state, const ResolvedKey& key, std::size_t newPackageIndex) {
    auto mv = state.scopeMoves.find(key);
    if (mv == state.scopeMoves.end()) return;
    for (auto& e : state.dependencyEdges)
        if (e.scope == key.scope && e.dependencyPackageIndex == mv->second.oldPackageIndex)
            e.dependencyPackageIndex = newPackageIndex;
    // An edge the move made equal to one already there is the same edge once.
    std::vector<DependencyEdge> kept;
    for (auto& e : state.dependencyEdges) {
        const bool seen = std::ranges::any_of(kept, [&](const DependencyEdge& k) {
            return k.consumerPackageIndex == e.consumerPackageIndex
                && k.dependencyPackageIndex == e.dependencyPackageIndex && k.visibility == e.visibility
                && k.buildTable == e.buildTable && k.devTable == e.devTable;
        });
        if (!seen) kept.push_back(std::move(e));
    }
    state.dependencyEdges = std::move(kept);
    if (auto rec = state.resolved.find(key); rec != state.resolved.end()) {
        rec->second.constraintsBy = mv->second.constraintsBy;
        rec->second.constraint.clear();
        for (auto const& [by, c] : rec->second.constraintsBy)
            rec->second.constraint += (rec->second.constraint.empty() ? "" : ",") + c;
    }
    state.scopeMoves.erase(mv);
}

// A PROGRAM'S EDGE FROM A PACKAGE OF THE TARGET TO A PACKAGE ONLY PROGRAMS HOLD
// (§21 A1). A program that walked a package the target also holds may resolve
// a dependency of it at another version; the edge is the program's, and the
// target's graph -- its instances, its links, its cache keys -- does not reach
// that version through it. The program's own graph is the build programs'
// sub-build, which resolves its closure from the program's pins.
void step4b_drop_program_edges_from_target(PrepareState& state) {
    const auto n = state.packages.size();
    std::vector<bool> targetHeld(n, false);
    if (n > 0) targetHeld[0] = true;
    for (std::size_t i = 1; i < n; ++i)
        if (state.packages[i].selectedMember) targetHeld[i] = true;
    for (auto const& [k, r] : state.resolved)
        if (k.scope.empty() && r.depIndex + 1 < n) targetHeld[r.depIndex + 1] = true;
    std::erase_if(state.dependencyEdges, [&](const DependencyEdge& e) {
        return !e.scope.empty() && !e.buildTable && e.consumerPackageIndex < n
            && e.dependencyPackageIndex < n && targetHeld[e.consumerPackageIndex]
            && !targetHeld[e.dependencyPackageIndex];
    });
}

} // namespace mcpp::build
