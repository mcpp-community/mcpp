// graph_requests.cpp -- P4b: what a request of the dependency worklist is bound
// by, and which side of the build makes it. Split from graph.cpp at the
// boundary of a request's own facts, when a request learned to carry the
// platform's version group into its merges and a package's tables came to be
// read by the row each of its instances is built for (mcpp#956; SPEC-013 §7.5,
// SPEC-004 §4.8).

module mcpp.build.prepare;
import :state;

import std;
import mcpp.build.prepare_inputs;
import mcpp.manifest;
import mcpp.pm.dep_spec;
import mcpp.toolchain.triple;

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
    return out;
}

} // namespace mcpp::build
