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

// What earlier runs measured: the newest duration of each program record, by
// its name, and of each case, by its program and its id (`case_key`): two
// programs may each have a case `layers`.
struct History {
    std::map<std::string, double> programs;
    std::map<std::string, double> cases;
    // A CASE DIVIDED IS MEASURED BOTH WAYS (P4.3), by `case_key`: `whole`, its
    // newest duration in a run that did not divide it; `divided`, the longest
    // part of the newest run that did. The cases of one case may share a state
    // the first of them fills (a cache, a toolchain), and every part pays it
    // again: e2e/openkal/sets took 1375 s whole and 909, 1109 and 1306 s as
    // three parts (run 43641874830). The runs say whether a division pays.
    std::map<std::string, double> whole;
    std::map<std::string, double> divided;
    std::size_t runs = 0;
    bool empty() const { return programs.empty() && cases.empty(); }
};

// The newest `maxRuns` runs of the store, by their start. With `shardsOf`,
// the runs of a shard of that group (the CI run, or none) are left out: the
// shards of one run, run one after another on one store, read the store as
// the first of them read it.
//
// With `machine`, the runs on that class of machine (SPEC-017 P12.4) when the
// store has one, else all: the processors of one hosted pool are 1.6 times
// apart (design 2026-11-20 §3.1). The shards of a run do not pass it: each
// shard lands on its own machine and must compute the one plan the others do.
History read_history(const std::filesystem::path& store, std::size_t maxRuns = 30,
                     std::optional<std::string> shardsOf = std::nullopt,
                     std::optional<std::string> machine = std::nullopt);

// The key of a case in `History::cases`: its program (the program a part is of)
// and its id.
std::string case_key(std::string_view program, std::string_view id);

// WHETHER A DIVISION OF CASE `id` PAYS: a case no run divided is divided once,
// to be measured so; then it is divided while its longest part took at most
// `kDivisionGain` of the case whole. With no whole duration in the window (a
// run of the store's newest that all divided it), it runs whole, to be
// measured again.
inline constexpr double kDivisionGain = 0.8;
bool division_pays(const History& h, std::string_view program, std::string_view id);

// THE CASES A CASE DECLARES, AS RUNS MEASURED THEM (P3.4: a list does not name
// them): the innermost below `id` in program `program`, in id order. A runner
// divides a long case by them (P4.3).
std::vector<std::string> measured_within(const History& h, std::string_view program, std::string_view id);

// The duration a run of `cases` of the program record `name` is expected to
// take; with no cases, the program run whole. A case written `!<id>` is left
// out of the cases that declare it (P4.3).
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
// into another) is one run. `started` is stated to the second, and two runs of
// one second are ordered as the index appended them: the later line is the
// newer run (a `--failed` right after a failing run read the passing run before
// it, cli::surface/1126).
std::vector<json> runs_newest_first(const std::filesystem::path& store) {
    auto runs = read_lines(store / "index.ndjson");
    std::ranges::reverse(runs);
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
                     std::optional<std::string> shardsOf, std::optional<std::string> machine) {
    History h;
    std::error_code ec;
    if (!std::filesystem::exists(store / "index.ndjson", ec)) return h;
    const auto runs = runs_newest_first(store);
    if (machine && std::ranges::none_of(runs, [&](const json& r) { return text(r, "machine") == *machine; }))
        machine.reset();
    for (const auto& run : runs) {
        if (h.runs == maxRuns) break;
        if (machine && text(run, "machine") != *machine) continue;
        if (shardsOf && !text(run, "shard").empty() && text(run, "group") == *shardsOf) continue;
        const auto dir = store / "runs" / text(run, "id");
        if (!std::filesystem::is_directory(dir, ec)) continue;
        ++h.runs;
        std::map<std::string, std::string> programOf;   // a part's record -> its program
        std::map<std::string, std::string> dividesOf;   // a part's record -> the case it divides
        std::map<std::string, double> longest;          // a divided case -> its longest part here
        for (const auto& p : read_lines(dir / "programs.ndjson")) {
            const auto of = text(p, "part_of");
            const auto program = of.empty() ? text(p, "name") : of;
            programOf[text(p, "name")] = program;
            const auto divides = text(p, "divides");
            if (!divides.empty()) dividesOf[text(p, "name")] = divides;
            const auto status = text(p, "status");
            const double ms = number(p, "ms");
            // A test built and not run, or not run, measured nothing.
            if (ms < 0 || status == "built" || status == "not-run") continue;
            h.programs.try_emplace(text(p, "name"), ms);
            if (!divides.empty()) {
                auto& l = longest[case_key(program, divides)];
                l = (std::max)(l, ms);
            }
        }
        for (const auto& [key, ms] : longest) h.divided.try_emplace(key, ms);
        for (const auto& c : read_lines(dir / "cases.ndjson")) {
            const double ms = number(c, "ms");
            if (ms < 0 || text(c, "status") == "skip") continue;
            const auto record = text(c, "program");
            const auto of = programOf.find(record);
            const auto id = text(c, "id");
            const auto key = case_key(of != programOf.end() ? of->second : record, id);
            h.cases.try_emplace(key, ms);
            // A part of a divided case measured that case, and the cases that
            // declare it, only in part.
            const auto d = dividesOf.find(record);
            const bool partial = d != dividesOf.end()
                && (d->second == id || d->second.starts_with(id + "/"));
            if (!partial) h.whole.try_emplace(key, ms);
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
    for (std::size_t i = 0; i < ids.size(); ++i) {
        // `?<id>` (P4.3) is the case where it is declared.
        const std::string_view id = ids[i].starts_with('?') ? std::string_view(ids[i]).substr(1) : ids[i];
        auto it = h.cases.find(case_key(name, id));
        if (it == h.cases.end()) continue;
        // A case run whole is what it took then; its declared cases measured
        // in parts each paid what they share.
        if (auto w = h.whole.find(case_key(name, id)); w != h.whole.end()) {
            est[i] = w->second;
            sum += est[i];
            ++known;
            continue;
        }
        // A case a run divided measured only its rest there: it takes at
        // least what the cases it declared took.
        double inner = 0;
        for (auto const& c : measured_within(h, name, id))
            if (auto m = h.cases.find(case_key(name, c)); m != h.cases.end()) inner += m->second;
        est[i] = (std::max)(it->second, inner);
        sum += est[i];
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

std::string case_key(std::string_view program, std::string_view id) {
    return std::format("{}\n{}", program, id);
}

bool division_pays(const History& h, std::string_view program, std::string_view id) {
    const auto key = case_key(program, id);
    const auto d = h.divided.find(key);
    if (d == h.divided.end()) return true;
    const auto w = h.whole.find(key);
    return w != h.whole.end() && d->second <= kDivisionGain * w->second;
}

std::vector<std::string> measured_within(const History& h, std::string_view program, std::string_view id) {
    const auto prefix = case_key(program, id) + "/";
    const auto skip = case_key(program, "").size();
    std::vector<std::string> keys;
    for (auto it = h.cases.lower_bound(prefix); it != h.cases.end() && it->first.starts_with(prefix); ++it)
        keys.push_back(it->first);
    std::vector<std::string> out;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        // A case that declares a measured case is not one of the innermost.
        if (i + 1 < keys.size() && keys[i + 1].starts_with(keys[i] + "/")) continue;
        out.push_back(keys[i].substr(skip));
    }
    return out;
}

double estimate(const History& h, std::string_view name, std::span<const std::string> cases) {
    if (cases.empty()) return program_estimate(h, name);
    std::vector<std::string> in, out;
    for (const auto& c : cases) (c.starts_with('!') ? out : in).push_back(c.starts_with('!') ? c.substr(1) : c);
    double sum = 0;
    for (double e : case_estimates(h, name, in)) sum += e;
    if (!out.empty())
        for (double e : case_estimates(h, name, out)) sum -= e;
    return (std::max)(sum, 0.0);
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
