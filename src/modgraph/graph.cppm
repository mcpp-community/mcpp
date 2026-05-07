// mcpp.modgraph.graph — DAG of module units (per-package, cross-package later).

export module mcpp.modgraph.graph;

import std;

export namespace mcpp::modgraph {

struct ModuleId {
    std::string                     logicalName;       // "mcpplibs.hello.core"

    auto operator<=>(const ModuleId&) const = default;
};

struct SourceUnit {
    std::filesystem::path           path;
    std::string                     packageName;
    std::optional<ModuleId>         provides;
    std::vector<ModuleId>           requires_;
    bool                            isModuleInterface = false;   // .cppm with export module
    bool                            isImplementation   = false;   // .cpp without export
};

struct Graph {
    std::vector<SourceUnit>                                  units;
    // logical-name -> index into units
    std::map<std::string, std::size_t, std::less<>>             producerOf;
    // edges as (consumer-index, producer-index)
    std::vector<std::pair<std::size_t, std::size_t>>         edges;
};

// Topological order: returns indices of units in producer-before-consumer order.
// Returns std::unexpected with the cycle if any.
struct CycleError {
    std::vector<std::size_t> cycle;
};
std::expected<std::vector<std::size_t>, CycleError> topo_sort(const Graph& g);

} // namespace mcpp::modgraph

namespace mcpp::modgraph {

std::expected<std::vector<std::size_t>, CycleError> topo_sort(const Graph& g) {
    std::vector<std::size_t> indeg(g.units.size(), 0);
    std::vector<std::vector<std::size_t>> adj(g.units.size());
    for (auto [c, p] : g.edges) {
        // edge means: consumer depends on producer. So producer must come first.
        // indegree of consumer counts unmet producer dependencies.
        indeg[c]++;
        adj[p].push_back(c);
    }

    std::vector<std::size_t> order;
    order.reserve(g.units.size());
    std::vector<std::size_t> queue;
    for (std::size_t i = 0; i < indeg.size(); ++i) {
        if (indeg[i] == 0) queue.push_back(i);
    }
    while (!queue.empty()) {
        std::size_t u = queue.back();
        queue.pop_back();
        order.push_back(u);
        for (auto v : adj[u]) {
            if (--indeg[v] == 0) queue.push_back(v);
        }
    }
    if (order.size() != g.units.size()) {
        // Cycle remains. Report units still with positive indegree.
        CycleError err;
        for (std::size_t i = 0; i < indeg.size(); ++i) {
            if (indeg[i] > 0) err.cycle.push_back(i);
        }
        return std::unexpected(err);
    }
    return order;
}

} // namespace mcpp::modgraph
