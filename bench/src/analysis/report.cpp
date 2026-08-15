// bench.analysis.report — implementation.
//
// `module bench.analysis.report;` with no `export`: an implementation unit, so nothing below
// reaches an importer's BMI. The critical-path walk lives here.
module bench.analysis.report;

import std;
import bench.analysis.ninjalog;
import bench.analysis.graph;

namespace bench::analysis {

namespace detail {

std::pair<std::int64_t, std::vector<std::string>>
longest_path(const Graph& g, const Log& log, const std::string& sink) {
    auto dur = [&](const std::string& n) -> std::int64_t {
        const auto* e = log.find(n);
        return e ? e->duration_ms() : 0;
    };

    std::unordered_map<std::string, std::size_t>              indeg;
    std::unordered_map<std::string, std::vector<std::string>> succ;
    std::unordered_set<std::string>                           nodes;

    for (const auto& [n, ds] : g.deps) {
        nodes.insert(n);
        for (const auto& d : ds) nodes.insert(d);
    }
    nodes.insert(sink);
    for (const auto& n : nodes) indeg.try_emplace(n, 0);
    for (const auto& [n, ds] : g.deps) {
        indeg[n] = ds.size();
        for (const auto& d : ds) succ[d].push_back(n);
    }

    std::unordered_map<std::string, std::int64_t> best;
    std::unordered_map<std::string, std::string>  from;
    std::vector<std::string> ready;
    for (const auto& [n, k] : indeg)
        if (k == 0) ready.push_back(n);

    std::size_t relaxed = 0;
    while (!ready.empty()) {
        auto n = ready.back();
        ready.pop_back();
        ++relaxed;
        std::int64_t b = 0;
        std::string  pick;
        if (auto it = g.deps.find(n); it != g.deps.end()) {
            for (const auto& d : it->second) {
                auto v = best.contains(d) ? best[d] : 0;
                if (v > b) { b = v; pick = d; }
            }
        }
        best[n] = b + dur(n);
        from[n] = pick;
        if (auto it = succ.find(n); it != succ.end())
            for (const auto& s : it->second)
                if (--indeg[s] == 0) ready.push_back(s);
    }
    if (relaxed != nodes.size()) {
        // A cycle would leave nodes unrelaxed; the graph should be acyclic, so
        // say so rather than silently reporting a short path.
        std::println(std::cerr,
                     "buildstat: warning — {} of {} nodes unrelaxed (cycle in the graph?); "
                     "critical path is a lower bound",
                     nodes.size() - relaxed, nodes.size());
    }

    std::vector<std::string> chain;
    for (auto n = sink; !n.empty();) {
        chain.push_back(n);
        auto it = from.find(n);
        n = (it == from.end()) ? std::string{} : it->second;
    }
    std::ranges::reverse(chain);
    return {best.contains(sink) ? best[sink] : 0, chain};
}

}  // namespace detail

}  // namespace bench::analysis
