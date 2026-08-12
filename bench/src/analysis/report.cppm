// Turn a parsed ninja log + graph into the four numbers that actually explain a
// modular C++ build's wall clock:
//
//   work        sum of every edge's duration        "how much CPU the build costs"
//   makespan    last end - first start              "what the user waited"
//   critical    longest dependency-weighted path    "what no amount of cores fixes"
//   concurrency work/makespan over time             "where the machine went idle"
//
// For mcpp's own build these read 309 s / 79 s / 79 s / 3.9x — critical path is
// 100% of makespan, so the build is latency-bound, not throughput-bound, and
// buying more cores buys nothing.
export module bench.analysis.report;

import std;
import bench.analysis.ninjalog;
import bench.analysis.graph;

export namespace bench::analysis {

struct RuleStat {
    std::string  rule;
    std::size_t  count{};
    std::int64_t total_ms{};
    std::int64_t max_ms{};
};

struct Analysis {
    std::int64_t              work_ms{};
    std::int64_t              makespan_ms{};
    std::int64_t              critical_ms{};
    std::vector<RuleStat>     rules;              // descending by total_ms
    std::vector<std::string>  critical_chain;     // node ids, source -> sink
    std::vector<double>       concurrency;        // per time bucket
};

namespace detail {

// Longest path by Kahn topological relaxation.
//
// A recursive/stack DFS is the obvious implementation and it is WRONG here in a
// way that is quiet: when a dependency is already on the traversal stack (pushed
// via a sibling branch) it must not be treated as resolved, but the natural
// "skip what is on the stack" cycle guard does exactly that and scores it 0. The
// walk then terminates early — on mcpp's own build it reported 33.9 s over 10
// nodes where the true answer is 79.0 s over 24, i.e. it turned a 100%-critical
// -path build into a 44% one and inverted the whole diagnosis.
//
// Topological order sidesteps it: a node is relaxed only once EVERY dependency
// has a final value.
inline std::pair<std::int64_t, std::vector<std::string>>
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

Analysis analyze(const Log& log, const Graph& g, std::size_t buckets = 20) {
    Analysis a;
    a.work_ms     = log.work_ms();
    a.makespan_ms = log.makespan_ms();

    std::map<std::string, RuleStat> byrule;
    for (const auto& e : log.edges) {
        auto it   = g.rule_of.find(e.id());
        auto name = it == g.rule_of.end() ? std::string("unknown") : it->second;
        auto& r   = byrule[name];
        r.rule    = name;
        ++r.count;
        r.total_ms += e.duration_ms();
        r.max_ms    = std::max(r.max_ms, e.duration_ms());
    }
    for (auto& [_, r] : byrule) a.rules.push_back(r);
    std::ranges::sort(a.rules, [](auto& x, auto& y) { return x.total_ms > y.total_ms; });

    // Sink = the link edge if there is one, else the latest-finishing edge.
    std::string sink;
    std::int64_t latest = std::numeric_limits<std::int64_t>::min();
    for (const auto& e : log.edges) {
        auto it = g.rule_of.find(e.id());
        if (it != g.rule_of.end() && it->second.contains("link")) { sink = e.id(); break; }
        if (e.end_ms > latest) { latest = e.end_ms; sink = e.id(); }
    }
    if (!sink.empty()) {
        auto [cost, chain] = detail::longest_path(g, log, sink);
        a.critical_ms      = cost;
        a.critical_chain   = std::move(chain);
    }

    // Concurrency: fraction of each time bucket covered by running edges.
    a.concurrency.assign(buckets, 0.0);
    if (a.makespan_ms > 0) {
        const double t0   = static_cast<double>(log.t0_ms());
        const double binw = static_cast<double>(a.makespan_ms) / static_cast<double>(buckets);
        for (const auto& e : log.edges) {
            double s = static_cast<double>(e.start_ms) - t0;
            double f = static_cast<double>(e.end_ms) - t0;
            for (std::size_t b = 0; b < buckets; ++b) {
                double bs = static_cast<double>(b) * binw, be = bs + binw;
                double ov = std::min(f, be) - std::max(s, bs);
                if (ov > 0) a.concurrency[b] += ov / binw;
            }
        }
    }
    return a;
}

void print_report(const Analysis& a, const Log& log, const Graph& g,
                  std::string_view label, std::size_t cores) {
    auto sec = [](std::int64_t ms) { return static_cast<double>(ms) / 1000.0; };
    std::println("### {}", label);
    std::println("edges          : {}", log.edges.size());
    std::println("makespan       : {:.2f} s", sec(a.makespan_ms));
    std::println("work (sum dur) : {:.2f} s", sec(a.work_ms));
    std::println("avg parallelism: {:.2f} x  (of {} hw threads)",
                 a.makespan_ms ? double(a.work_ms) / double(a.makespan_ms) : 0.0, cores);
    std::println("critical path  : {:.2f} s  = {:.0f}% of makespan", sec(a.critical_ms),
                 a.makespan_ms ? 100.0 * double(a.critical_ms) / double(a.makespan_ms) : 0.0);
    std::println("");
    std::println("{:<16}{:>7}{:>10}{:>9}{:>9}{:>8}", "rule", "count", "total_s", "avg_ms",
                 "max_ms", "%work");
    for (const auto& r : a.rules) {
        std::println("{:<16}{:>7}{:>10.2f}{:>9.1f}{:>9}{:>7.1f}%", r.rule, r.count,
                     sec(r.total_ms), double(r.total_ms) / double(r.count), r.max_ms,
                     a.work_ms ? 100.0 * double(r.total_ms) / double(a.work_ms) : 0.0);
    }

    std::println("");
    std::println("critical chain ({} nodes, non-zero shown):", a.critical_chain.size());
    for (const auto& n : a.critical_chain) {
        const auto* e = log.find(n);
        if (!e || e->duration_ms() == 0) continue;
        auto it = g.rule_of.find(n);
        std::println("   {:>7} ms  {:<12} {}", e->duration_ms(),
                     it == g.rule_of.end() ? "?" : it->second, n);
    }

    std::println("");
    std::println("concurrency over time:");
    const double binw = double(a.makespan_ms) / double(a.concurrency.size()) / 1000.0;
    for (std::size_t b = 0; b < a.concurrency.size(); ++b) {
        auto bars = static_cast<int>(std::llround(a.concurrency[b] * 60.0 / double(cores)));
        std::println("  t={:>6.1f}s {:>5.1f}x |{}", double(b) * binw, a.concurrency[b],
                     std::string(static_cast<std::size_t>(std::max(0, bars)), '#'));
    }

    std::vector<const Edge*> slow;
    for (const auto& e : log.edges) slow.push_back(&e);
    std::ranges::sort(slow, [](auto* x, auto* y) { return x->duration_ms() > y->duration_ms(); });
    std::println("");
    std::println("slowest edges:");
    for (std::size_t i = 0; i < std::min<std::size_t>(12, slow.size()); ++i) {
        auto it = g.rule_of.find(slow[i]->id());
        std::println("   {:>7} ms  {:<12} {}", slow[i]->duration_ms(),
                     it == g.rule_of.end() ? "?" : it->second, slow[i]->id());
    }
}

}  // namespace bench::analysis
