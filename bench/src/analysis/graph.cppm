// Reconstruct the dependency graph ninja actually executed.
//
// Two sources, and BOTH are required:
//   1. build.ninja        — static edges, rule names
//   2. obj/**/*.ddi.dd    — the dyndep files, which is where every real
//                           `import` edge lives for a C++ modules build
//
// Reading only build.ninja makes mcpp's critical path measure 22 s instead of
// 79 s: the module graph is invisible until dyndep is folded in.
//
// A second subtlety costs another 3x: dyndep attaches its deps to `obj/X.m.o`,
// but importers depend on the OTHER output of that same edge,
// `gcm.cache/X.gcm`. Unless all outputs of an edge are one graph node, the
// longest-path walk terminates after a couple of hops.
export module bench.analysis.graph;

import std;
import bench.analysis.ninjalog;

export namespace bench::analysis {

struct Graph {
    // node identity == the ninja Edge identity (its first sorted output)
    std::unordered_map<std::string, std::unordered_set<std::string>> deps;
    std::unordered_map<std::string, std::string>                     rule_of;
    std::unordered_map<std::string, std::string>                     node_of;  // output -> node

    [[nodiscard]] std::string resolve(const std::string& output) const {
        auto it = node_of.find(output);
        return it == node_of.end() ? std::string{} : it->second;
    }
};

namespace detail {

std::vector<std::string> split_ws(std::string_view s);

// A ninja `build` statement: `build OUT... [| IMPLICIT_OUT...] : RULE IN... [| IMP] [|| ORDER]`
struct Stmt {
    std::vector<std::string> outs;
    std::string              rule;
    std::vector<std::string> ins;
};

std::optional<Stmt> parse_build(std::string_view line);

// ninja continues a logical line with a trailing `$`.
std::string read_unfolded(const std::filesystem::path& p);

}  // namespace detail

Graph build_graph(const std::filesystem::path& build_dir, const Log& log) {
    Graph g;

    // Every output of a timed edge collapses onto that edge's identity.
    for (const auto& e : log.edges)
        for (const auto& o : e.outputs) g.node_of[o] = e.id();

    auto ingest = [&](const std::string& text, bool dyndep_only) {
        std::size_t pos = 0;
        while (pos <= text.size()) {
            auto nl   = text.find('\n', pos);
            auto line = std::string_view(text).substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

            auto st = detail::parse_build(line);
            if (!st) continue;
            if (dyndep_only && st->rule != "dyndep") continue;

            // node identity: prefer the timed-edge identity, else invent one
            std::string node;
            for (const auto& o : st->outs)
                if (auto it = g.node_of.find(o); it != g.node_of.end()) { node = it->second; break; }
            if (node.empty()) node = *std::ranges::min_element(st->outs);
            for (const auto& o : st->outs) g.node_of.emplace(o, node);
            if (!dyndep_only) g.rule_of[node] = st->rule;

            for (const auto& in : st->ins) g.deps[node].insert(in);
        }
    };

    ingest(detail::read_unfolded(build_dir / "build.ninja"), false);

    std::error_code ec;
    auto objdir = build_dir / "obj";
    if (std::filesystem::exists(objdir, ec)) {
        for (auto const& de : std::filesystem::recursive_directory_iterator(objdir, ec)) {
            if (de.is_regular_file(ec) && de.path().extension() == ".dd")
                ingest(detail::read_unfolded(de.path()), true);
        }
    }

    // Re-map every dependency name onto its node identity; drop self-edges and
    // leaves (source files produced by nothing).
    std::unordered_map<std::string, std::unordered_set<std::string>> mapped;
    for (auto& [node, ins] : g.deps) {
        auto& set = mapped[node];
        for (const auto& in : ins) {
            auto it = g.node_of.find(in);
            if (it != g.node_of.end() && it->second != node) set.insert(it->second);
        }
    }
    g.deps = std::move(mapped);
    return g;
}

}  // namespace bench::analysis
