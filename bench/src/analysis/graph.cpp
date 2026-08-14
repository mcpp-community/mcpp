// bench.analysis.graph — implementation.
//
// `module bench.analysis.graph;` with no `export`: an implementation unit, so nothing below
// reaches an importer's BMI. The ninja-file parser lives here.
module bench.analysis.graph;

import std;
import bench.analysis.ninjalog;

namespace bench::analysis {

namespace detail {

std::vector<std::string> split_ws(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        auto b = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        if (i > b) out.emplace_back(s.substr(b, i - b));
    }
    return out;
}

std::optional<Stmt> parse_build(std::string_view line) {
    constexpr std::string_view kw = "build ";
    if (!line.starts_with(kw)) return std::nullopt;
    auto body  = line.substr(kw.size());
    auto colon = body.find(':');
    if (colon == std::string_view::npos) return std::nullopt;

    Stmt st;
    for (auto& t : split_ws(body.substr(0, colon)))
        if (t != "|") st.outs.push_back(t);

    auto toks = split_ws(body.substr(colon + 1));
    if (toks.empty()) return std::nullopt;
    st.rule = toks.front();
    // Order-only deps still gate scheduling, so they are kept as real edges.
    for (std::size_t i = 1; i < toks.size(); ++i)
        if (toks[i] != "|" && toks[i] != "||") st.ins.push_back(toks[i]);
    if (st.outs.empty()) return std::nullopt;
    return st;
}

std::string read_unfolded(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in) return {};
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    out.reserve(all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i] == '$' && i + 1 < all.size() && all[i + 1] == '\n') { out += ' '; ++i; }
        else out += all[i];
    }
    return out;
}

}  // namespace detail

}  // namespace bench::analysis
