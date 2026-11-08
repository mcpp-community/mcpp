// graph_edges.cpp -- P4b: the edges the dependency worklist records, one per
// consumer, dependency, visibility and table, with what each edge asks of its
// dependency. Split from graph.cpp at the boundary of what an edge is, when the
// edge learned to carry `embed' and `exclude' (design 2026-11-02 §14.1).

module mcpp.build.prepare;
import :state;

import std;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.pm.dep_spec;

namespace mcpp::build {

static mcpp::modgraph::DependencyVisibility parseVisibility(std::string_view visibility) {
        if (visibility == "private")
            return mcpp::modgraph::DependencyVisibility::Private;
        if (visibility == "interface")
            return mcpp::modgraph::DependencyVisibility::Interface;
        return mcpp::modgraph::DependencyVisibility::Public;
}

std::size_t packageIndexForConsumer(std::size_t consumerDepIndex) {
        if (consumerDepIndex == kMainConsumer) return std::size_t{0};
        return consumerDepIndex + 1;
}

void recordDependencyEdge(
        PrepareState& state,
        std::size_t consumerDepIndex,
        std::size_t dependencyPackageIndex,
        const mcpp::manifest::DependencySpec& spec,
        bool buildTable,
        const std::string& writtenKey)
{
        const auto consumerPackageIndex = packageIndexForConsumer(consumerDepIndex);
        if (consumerPackageIndex >= state.packages.size()
            || dependencyPackageIndex >= state.packages.size()) {
            return;
        }
        if (std::ranges::none_of(state.graphRequests, [&](const GraphRequest& r) {
                return r.consumerPackageIndex == consumerPackageIndex
                    && r.dependencyPackageIndex == dependencyPackageIndex
                    && r.key == writtenKey && r.table == spec.declaredIn;
            }))
            state.graphRequests.push_back(GraphRequest{
                .consumerPackageIndex = consumerPackageIndex,
                .dependencyPackageIndex = dependencyPackageIndex,
                .key = writtenKey,
                .table = spec.declaredIn,
            });

        // A REQUESTED FEATURE THAT IS A BUILD RULE IMPLIES THE LIBRARY.
        //
        // A feature declaring `rule_module` is usable only by a build program,
        // so requesting it asks for the dependency's library there. Requiring
        // `host-module` beside it was a second spelling of one fact.
        bool ruleFeature = false;
        {
            auto const& depManifest = state.packages[dependencyPackageIndex].manifest;
            for (auto const& f : spec.features)
                if (depManifest.featureRuleModule.contains(f)) { ruleFeature = true; break; }
        }
        // COMPAT(host-module-in-dependencies): a `[dependencies]` entry asking
        // for `host-module = true`, or for a rule feature, served the build
        // program and not the target before the section decided the
        // configuration (mcpp#797). It keeps that meaning, reported where the
        // manifest can be edited.
        // Not in the build programs' graph: every package there is a build-program
        // instance already, and a library's rule features are what its edge asks.
        if (!buildTable && !state.overrides.build_program_graph
            && ((spec.hostModuleDeclared && spec.hostModule) || ruleFeature)) {
            buildTable = true;
            state.reportCompatUse("host-module-in-dependencies", consumerPackageIndex,
                                  std::format("[dependencies] {}", writtenKey));
        }
        const bool wantsLibrary = buildTable
            && (ruleFeature
                || (spec.hostModuleDeclared ? spec.hostModule
                                            : spec.tools.empty() && spec.artifacts.empty()));

        // One edge per (consumer, dependency, visibility, configuration). The
        // same package written in [dependencies] and [build-dependencies] of
        // one consumer is two edges and two instances (mcpp#797, R2): merging
        // them made the build-program request read as "not in the target".
        const auto visibility = parseVisibility(spec.visibility);
        const bool devTable = !buildTable && spec.declaredIn.ends_with("dev-dependencies]");
        auto same = [&](const DependencyEdge& edge) {
            return edge.consumerPackageIndex == consumerPackageIndex
                && edge.dependencyPackageIndex == dependencyPackageIndex
                && edge.visibility == visibility
                && edge.buildTable == buildTable
                && edge.devTable == devTable;
        };
        auto it = std::find_if(state.dependencyEdges.begin(), state.dependencyEdges.end(), same);
        if (it != state.dependencyEdges.end()) {
            // One consumer restating one dependency in one table (an
            // unconditional entry and a `[target.<sel>.*]` or `[feature-deps]`
            // row): every declaration's requests are requested of the one edge
            // (#649 E7). Additive fields union, `default-features` stays on
            // unless every declaration opts out.
            for (auto const& t : spec.tools)
                if (std::ranges::find(it->requestedTools, t) == it->requestedTools.end())
                    it->requestedTools.push_back(t);
            for (auto const& a : spec.artifacts)
                if (std::ranges::find(it->requestedArtifacts, a)
                    == it->requestedArtifacts.end())
                    it->requestedArtifacts.push_back(a);
            for (auto const& f : spec.features)
                if (std::ranges::find(it->requestedFeatures, f)
                    == it->requestedFeatures.end())
                    it->requestedFeatures.push_back(f);
            it->defaultFeatures = it->defaultFeatures || spec.defaultFeatures;
            it->wantsLibrary = it->wantsLibrary || wantsLibrary;
            it->reexport = it->reexport || spec.reexport;
            it->embed = it->embed || spec.embed;
            for (auto const& x : spec.exclude)
                if (std::ranges::find(it->exclude, x) == it->exclude.end()) it->exclude.push_back(x);
            return;
        }
        state.dependencyEdges.push_back(DependencyEdge{
            .consumerPackageIndex = consumerPackageIndex,
            .dependencyPackageIndex = dependencyPackageIndex,
            .visibility = visibility,
            .requestedFeatures = spec.features,
            .defaultFeatures = spec.defaultFeatures,
            .requestedTools = spec.tools,
            .requestedArtifacts = spec.artifacts,
            .buildTable = buildTable,
            .devTable = devTable,
            .wantsLibrary = wantsLibrary,
            .reexport = spec.reexport,
            .embed = spec.embed,
            .exclude = spec.exclude,
        });
}

} // namespace mcpp::build
