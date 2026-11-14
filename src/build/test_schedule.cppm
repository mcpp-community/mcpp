// mcpp.build.test_schedule -- the order, the division and the shards of a
// `mcpp test` run, from the durations the run store measured (docs/08 "The
// order of a run"; design 2026-11-14). The store is the mtest store `[test]
// store` names: `index.ndjson`, and per run `runs/<id>/programs.ndjson` and
// `cases.ndjson`. Everything here is a function of the store and the run's
// list, so that every shard of one run, reading one store, computes one plan.
export module mcpp.build.test_schedule;

import std;
import nlohmann.json;

export namespace mcpp::build::test_schedule {

// What earlier runs measured: the newest duration of each program record and
// of each case, by name and by case id.
struct History {
    std::map<std::string, double> programs;
    std::map<std::string, double> cases;
    std::size_t runs = 0;
    bool empty() const { return programs.empty() && cases.empty(); }
};

// The newest `maxRuns` runs of the store, by their start. With `shardsOf`,
// the runs of a shard of that group (the CI run, or none) are left out: the
// shards of one run, run one after another on one store, read the store as
// the first of them read it.
History read_history(const std::filesystem::path& store, std::size_t maxRuns = 30,
                     std::optional<std::string> shardsOf = std::nullopt);

// The duration a run of `cases` of the program record `name` is expected to
// take; with no cases, the program run whole.
//
// WHAT NO RUN MEASURED. A program no run measured takes the mean of the
// measured programs of its nearest directory (`e2e/run/x`: of `e2e/run/`,
// then of `e2e/`); with none measured there, the longest measured program, so
// that tests a history never saw (a store kept by a run of another selection)
// are spread over the shards by count rather than gathered in one. A case no
// run measured takes the mean of the measured cases beside it; with none, an
// equal part of its program's estimate.
double estimate(const History& h, std::string_view name, std::span<const std::string> cases);

// LONGEST PROCESSING TIME FIRST: each item, longest first (ties by name),
// goes to the bin with the least work. Returns the bin of each item.
std::vector<std::size_t> assign(std::span<const double> estimates, std::span<const std::string> names,
                                std::size_t bins);

// The cases `ids` of the program record `name` divided into `k` groups of
// about equal measured duration, each group in the order the cases were listed.
std::vector<std::vector<std::string>> divide(const History& h, std::string_view name,
                                             std::span<const std::string> ids, std::size_t k);

// `I/N`, 1 <= I <= N.
std::optional<std::pair<int, int>> parse_shard(std::string_view text);

// THE FAILURES OF THE LAST RUN (`mcpp test --failed`): the programs that
// failed, and the cases that failed in them. `whole` is set when a failed
// program names no failed case (it implements no protocol, or failed outside
// its cases), so its program runs whole.
struct Failures {
    std::string run;
    std::vector<std::string> programs;
    std::vector<std::string> cases;
    bool whole = false;
};
std::optional<Failures> last_failures(const std::filesystem::path& store);

}  // namespace mcpp::build::test_schedule

namespace mcpp::build::test_schedule {

namespace {

using nlohmann::json;

std::vector<json> read_lines(const std::filesystem::path& file) {
    std::vector<json> out;
    std::ifstream in(file, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line == "\r") continue;
        auto j = json::parse(line, nullptr, false);
        if (j.is_object()) out.push_back(std::move(j));
    }
    return out;
}

double number(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : -1;
}

std::string text(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// The runs of the index, newest first; a run listed twice (a store imported
// into another) is one run.
std::vector<json> runs_newest_first(const std::filesystem::path& store) {
    auto runs = read_lines(store / "index.ndjson");
    std::ranges::stable_sort(runs, std::greater{}, [](const json& r) { return text(r, "started"); });
    std::set<std::string> seen;
    std::vector<json> out;
    for (auto& r : runs)
        if (auto id = text(r, "id"); !id.empty() && seen.insert(id).second) out.push_back(std::move(r));
    return out;
}

bool failed_case(std::string_view status) { return status == "fail" || status == "xpass"; }

}  // namespace

History read_history(const std::filesystem::path& store, std::size_t maxRuns,
                     std::optional<std::string> shardsOf) {
    History h;
    std::error_code ec;
    if (!std::filesystem::exists(store / "index.ndjson", ec)) return h;
    for (const auto& run : runs_newest_first(store)) {
        if (h.runs == maxRuns) break;
        if (shardsOf && !text(run, "shard").empty() && text(run, "group") == *shardsOf) continue;
        const auto dir = store / "runs" / text(run, "id");
        if (!std::filesystem::is_directory(dir, ec)) continue;
        ++h.runs;
        for (const auto& p : read_lines(dir / "programs.ndjson")) {
            const auto status = text(p, "status");
            const double ms = number(p, "ms");
            // A test built and not run, or not run, measured nothing.
            if (ms < 0 || status == "built" || status == "not-run") continue;
            h.programs.try_emplace(text(p, "name"), ms);
        }
        for (const auto& c : read_lines(dir / "cases.ndjson")) {
            const double ms = number(c, "ms");
            if (ms >= 0 && text(c, "status") != "skip") h.cases.try_emplace(text(c, "id"), ms);
        }
    }
    return h;
}

namespace {

// A program no run measured: the mean of its nearest measured directory, else
// the longest measured program (see `estimate`).
double unmeasured_program(const History& h, std::string_view name) {
    if (h.programs.empty()) return 1000;
    for (auto slash = name.rfind('/'); slash != std::string_view::npos && slash > 0;
         slash = name.rfind('/', slash - 1)) {
        const auto dir = std::string(name.substr(0, slash + 1));
        double sum = 0;
        std::size_t n = 0;
        for (auto it = h.programs.lower_bound(dir); it != h.programs.end() && it->first.starts_with(dir); ++it) {
            sum += it->second;
            ++n;
        }
        if (n) return sum / static_cast<double>(n);
    }
    return std::ranges::max(h.programs | std::views::values);
}

double program_estimate(const History& h, std::string_view name) {
    auto it = h.programs.find(std::string(name));
    return it != h.programs.end() ? it->second : unmeasured_program(h, name);
}

// Each case's estimate (see `estimate`).
std::vector<double> case_estimates(const History& h, std::string_view name, std::span<const std::string> ids) {
    std::vector<double> est(ids.size(), -1);
    double sum = 0;
    std::size_t known = 0;
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (auto it = h.cases.find(ids[i]); it != h.cases.end()) {
            est[i] = it->second;
            sum += it->second;
            ++known;
        }
    if (known == ids.size()) return est;
    const double each = known ? sum / static_cast<double>(known)
                              : program_estimate(h, name) / static_cast<double>(ids.size());
    for (auto& e : est)
        if (e < 0) e = each;
    return est;
}

}  // namespace

double estimate(const History& h, std::string_view name, std::span<const std::string> cases) {
    if (cases.empty()) return program_estimate(h, name);
    double sum = 0;
    for (double e : case_estimates(h, name, cases)) sum += e;
    return sum;
}

std::vector<std::size_t> assign(std::span<const double> estimates, std::span<const std::string> names,
                                std::size_t bins) {
    std::vector<std::size_t> order(estimates.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
        if (estimates[a] != estimates[b]) return estimates[a] > estimates[b];
        return names[a] < names[b];
    });
    std::vector<double> load((std::max)(bins, std::size_t{1}), 0.0);
    std::vector<std::size_t> bin(estimates.size(), 0);
    for (auto i : order) {
        const auto least = static_cast<std::size_t>(std::ranges::min_element(load) - load.begin());
        bin[i] = least;
        load[least] += estimates[i];
    }
    return bin;
}

std::vector<std::vector<std::string>> divide(const History& h, std::string_view name,
                                             std::span<const std::string> ids, std::size_t k) {
    k = std::clamp<std::size_t>(k, 1, (std::max)(ids.size(), std::size_t{1}));
    const auto est = case_estimates(h, name, ids);
    std::vector<std::string> names(ids.begin(), ids.end());
    const auto bin = assign(est, names, k);
    std::vector<std::vector<std::string>> groups(k);
    for (std::size_t i = 0; i < ids.size(); ++i) groups[bin[i]].push_back(ids[i]);
    std::erase_if(groups, [](const auto& g) { return g.empty(); });
    return groups;
}

std::optional<std::pair<int, int>> parse_shard(std::string_view s) {
    const auto slash = s.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    int i = 0, n = 0;
    auto a = s.substr(0, slash), b = s.substr(slash + 1);
    auto [pa, ea] = std::from_chars(a.data(), a.data() + a.size(), i);
    auto [pb, eb] = std::from_chars(b.data(), b.data() + b.size(), n);
    if (ea != std::errc{} || eb != std::errc{} || pa != a.data() + a.size() || pb != b.data() + b.size())
        return std::nullopt;
    if (n < 1 || i < 1 || i > n) return std::nullopt;
    return std::pair{i, n};
}

std::optional<Failures> last_failures(const std::filesystem::path& store) {
    std::error_code ec;
    if (!std::filesystem::exists(store / "index.ndjson", ec)) return std::nullopt;
    for (const auto& run : runs_newest_first(store)) {
        const auto dir = store / "runs" / text(run, "id");
        if (!std::filesystem::is_directory(dir, ec)) continue;
        Failures f;
        f.run = text(run, "id");
        std::map<std::string, std::string> rootOf;   // a record's name -> its program
        std::set<std::string> programs, withCases;
        for (const auto& p : read_lines(dir / "programs.ndjson")) {
            const auto name = text(p, "name");
            auto root = text(p, "part_of");
            if (root.empty()) root = name;
            rootOf[name] = root;
            const auto status = text(p, "status");
            if (status == "fail" || status == "build-fail") programs.insert(name);
        }
        std::set<std::string> cases;
        for (const auto& c : read_lines(dir / "cases.ndjson")) {
            if (!failed_case(text(c, "status"))) continue;
            const auto program = text(c, "program");
            cases.insert(text(c, "id"));
            withCases.insert(program);
            programs.insert(program);
        }
        std::set<std::string> roots;
        for (const auto& p : programs) {
            roots.insert(rootOf.contains(p) ? rootOf[p] : p);
            if (!withCases.contains(p)) f.whole = true;
        }
        f.programs.assign(roots.begin(), roots.end());
        f.cases.assign(cases.begin(), cases.end());
        return f;
    }
    return std::nullopt;
}

}  // namespace mcpp::build::test_schedule
