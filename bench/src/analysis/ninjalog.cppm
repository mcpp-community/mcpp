// Parse ninja's `.ninja_log` into per-EDGE timings.
//
// Format (v5/v6): start_ms \t end_ms \t mtime \t output \t command_hash
//
// The trap: an edge with several outputs (`build a.o | a.gcm : cxx_module ...`)
// writes ONE LINE PER OUTPUT, all sharing start/end/hash. Summing the lines
// double-counts the compile phase — for mcpp's own build that inflates module
// compile time from 302 s to 604 s. Edges are therefore keyed by
// (start, end, hash) and every output of an edge collapses onto one identity.
export module bench.analysis.ninjalog;

import std;

export namespace bench::analysis {

struct Edge {
    std::int64_t              start_ms{};
    std::int64_t              end_ms{};
    std::string               hash;
    std::vector<std::string>  outputs;   // sorted; outputs[0] is the identity

    [[nodiscard]] std::int64_t   duration_ms() const { return end_ms - start_ms; }
    [[nodiscard]] const std::string& id() const { return outputs.front(); }
};

struct Log {
    std::vector<Edge>                        edges;
    // `import std;` exports std::size_t but no global ::size_t — unqualified
    // spellings that a headers build would accept do not compile here.
    std::unordered_map<std::string, std::size_t>  edge_of_output;   // output -> index

    [[nodiscard]] const Edge* find(const std::string& output) const {
        auto it = edge_of_output.find(output);
        return it == edge_of_output.end() ? nullptr : &edges[it->second];
    }
    [[nodiscard]] std::int64_t makespan_ms() const {
        if (edges.empty()) return 0;
        auto lo = std::numeric_limits<std::int64_t>::max();
        auto hi = std::numeric_limits<std::int64_t>::min();
        for (const auto& e : edges) { lo = std::min(lo, e.start_ms); hi = std::max(hi, e.end_ms); }
        return hi - lo;
    }
    [[nodiscard]] std::int64_t work_ms() const {
        std::int64_t t = 0;
        for (const auto& e : edges) t += e.duration_ms();
        return t;
    }
    [[nodiscard]] std::int64_t t0_ms() const {
        auto lo = std::numeric_limits<std::int64_t>::max();
        for (const auto& e : edges) lo = std::min(lo, e.start_ms);
        return edges.empty() ? 0 : lo;
    }
};

// ninja APPENDS to .ninja_log and restarts its clock at 0 on every invocation, so
// a log touched by several builds holds overlapping time ranges. Mixing them
// yields a makespan SHORTER than the critical path (>100%) — that ratio is the
// tell that this filtering was skipped. Entries are written on completion, so
// `end` is non-decreasing within one run; a decrease starts a newer run.
std::expected<Log, std::string> parse_ninja_log(const std::filesystem::path& file,
                                                bool last_run_only = true) {
    std::ifstream in(file);
    if (!in) return std::unexpected("cannot open " + file.string());

    struct Row { std::int64_t s, e; std::string out, hash; };
    std::vector<Row> rows;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '#') continue;
        std::vector<std::string> f;
        for (std::size_t p = 0; p <= line.size();) {
            auto tab = line.find('\t', p);
            if (tab == std::string::npos) { f.emplace_back(line.substr(p)); break; }
            f.emplace_back(line.substr(p, tab - p));
            p = tab + 1;
        }
        if (f.size() < 5) continue;
        std::int64_t s{}, e{};
        auto [p1, ec1] = std::from_chars(f[0].data(), f[0].data() + f[0].size(), s);
        auto [p2, ec2] = std::from_chars(f[1].data(), f[1].data() + f[1].size(), e);
        if (ec1 != std::errc{} || ec2 != std::errc{}) continue;
        rows.push_back({s, e, f[3], f[4]});
    }

    std::size_t begin = 0;
    if (last_run_only) {
        for (std::size_t i = 1; i < rows.size(); ++i)
            if (rows[i].e < rows[i - 1].e) begin = i;
    }

    std::map<std::tuple<std::int64_t, std::int64_t, std::string>,
             std::vector<std::string>> grouped;
    for (std::size_t i = begin; i < rows.size(); ++i)
        grouped[{rows[i].s, rows[i].e, rows[i].hash}].push_back(rows[i].out);

    Log log;
    log.edges.reserve(grouped.size());
    for (auto& [key, outs] : grouped) {
        auto& [s, e, h] = key;
        Edge edge{s, e, h, outs};
        std::ranges::sort(edge.outputs);
        log.edges.push_back(std::move(edge));
    }
    for (std::size_t i = 0; i < log.edges.size(); ++i)
        for (const auto& o : log.edges[i].outputs)
            log.edge_of_output[o] = i;
    return log;
}

}  // namespace bench::analysis
