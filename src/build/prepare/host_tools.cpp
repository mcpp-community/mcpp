// host_tools.cpp -- what a host tool's store entry holds besides the tool: the
// tree it was built from, a user of the entry (mcpp#1025), and the shared
// objects of the graph it loads (mcpp#535). Declared in `:state`; the steps
// that build and publish a tool are in features.cpp.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.build.plan;

namespace mcpp::build {

std::filesystem::path tool_source_tree(const PrepareState& state, std::size_t depIdx) {
    if (depIdx < 1 || depIdx - 1 >= state.dep_cache_identities.size()
        || state.dep_cache_identities[depIdx - 1].sourceKind != "path")
        return {};
    const auto& id = state.dep_cache_identities[depIdx - 1];
    return id.sourceRef.empty() ? state.packages[depIdx].root : std::filesystem::path(id.sourceRef);
}

// THE TOOL IS AN IMAGE LIKE ANY OTHER (mcpp#535): what its link read from the
// sub-build's `bin/` it loads from beside itself (`$ORIGIN` is in its run
// path). The shared libraries of the graph it reaches -- through the link
// inputs of each, transitively -- and their run-time names are published with
// it. Before, the entry held the executable alone, and a tool over a `shared`
// package failed to start on a clean machine, or started with a library of the
// same name another installation had left in the environment.
std::vector<std::filesystem::path> tool_shared_objects(const mcpp::build::BuildPlan& plan,
                                                       const std::filesystem::path& goal) {
    std::vector<std::filesystem::path> out;
    std::vector<const LinkUnit*> work;
    for (auto const& lu : plan.linkUnits)
        if (lu.output == goal) work.push_back(&lu);
    std::set<std::filesystem::path> seen;
    while (!work.empty()) {
        const auto* at = work.back();
        work.pop_back();
        for (auto const& input : at->implicitInputs) {
            auto lu = std::ranges::find_if(plan.linkUnits, [&](const LinkUnit& u) {
                return u.kind == LinkUnit::SharedLibrary && u.output == input;
            });
            if (lu == plan.linkUnits.end() || !seen.insert(lu->output).second) continue;
            out.push_back(lu->output);
            for (auto const& alias : lu->runtimeAliases) out.push_back(alias);
            work.push_back(&*lu);
        }
    }
    return out;
}

} // namespace mcpp::build
