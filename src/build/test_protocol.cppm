// mcpp.build.test_protocol — the runner's side of the mtest test protocol,
// version 1 (mtest spec/protocol.md; docs/08 "Cases"). mcpp implements the
// protocol and not a framework: a test program that implements it (its entry
// states `test-protocol = 1`, or its file carries the marker, P2.3) is told the
// protocol's variables (P2.1), lists its cases (P3), runs a selection (P4) and
// reports each case (P5); every other program remains one case judged by its
// exit status (P1.1), and is told none of the variables (P1.2).

export module mcpp.build.test_protocol;

import std;
import nlohmann.json;

export namespace mcpp::build::test_protocol {

// The version this mcpp speaks.
inline constexpr int kVersion = 1;

// The framework mcpp scaffolds tests with and suggests where a package has
// none (docs/01, docs/08): mtest, by this release, from its repository.
inline constexpr std::string_view kMtestRelease = "0.8.5";
inline constexpr std::string_view kMtestRepo    = "https://github.com/mcpp-community/mtest";

// The protocol's variables, removed from the environment a program inherits
// so that an outer runner's do not reach a program mcpp starts (P2.2).
inline constexpr std::array<std::string_view, 7> kVariables = {
    "MTEST_PROTOCOL", "MTEST_MODE", "MTEST_REPORT", "MTEST_SELECT",
    "MTEST_ISOLATION", "MTEST_TMPDIR", "MTEST_SEED",
};

// The version the marker in a program's file states (P2.3): the bytes
// `mtest-protocol=` and a version followed by a byte that is not a digit; 0
// when the file carries none or cannot be read. A file may carry the bytes as
// data too (a test of a runner holds `mtest-protocol=12` among its cases), so
// the version this runner speaks is taken wherever it occurs, else the first.
int marker_version(const std::filesystem::path& program);

// A limit a case states for one of its metrics (P6.5): either may be absent.
struct Bound {
    std::string           metric;
    std::optional<double> max, min;
};

// A metric a case reported (P6.1): its name, value and unit, the samples it
// summarizes, which direction is better and how exactly it was measured.
struct Metric {
    std::string         name;
    double              value = 0;
    std::string         unit;
    std::vector<double> samples;
    std::string         better;      // lower, higher; empty: unstated
    std::string         precision;   // P6.6; empty: unstated
};

// What a program used of the machine, as it reports at its end (P10.2).
struct Usage {
    bool                         set = false;
    double                       userMs = 0, sysMs = 0;
    std::optional<std::uint64_t> peakBytes, readBytes, writtenBytes;
};

// What a program's case is, as its listing states it (P3.3).
struct Case {
    std::string              id;
    std::string              name;
    std::string              file;
    int                      line = 0;
    std::string              kind;
    std::vector<std::string> tags;
    std::string              expect;
    std::vector<Bound>       bounds;    // P6.5
    std::vector<std::string> resources; // P7.1: `machine` holds the machine alone
    double                   timeout = 0; // seconds the case states it may run; 0: none stated
};

// A failure an expectation of a case reported (P5.2).
struct Failure {
    std::string file;
    int         line = 0;
    std::string message;
    std::string expected;
    std::string actual;
    // SPEC-017 O3: the expectation's source statement, and whether the failure
    // ended the case.
    std::string source;
    bool        fatal = false;
};

// SPEC-017 O3: of the expectations a case ran, how many held and how many did
// not, as its `end` event states them.
struct Checks {
    long long held = 0;
    long long failed = 0;
};

// One case's result (P5.3), with its failures, its metrics and the limits it
// states for them.
struct CaseResult {
    std::string              id;
    std::string              status;   // pass, fail, skip, xfail, xpass, invalid
    double                   ms = 0;
    std::string              reason;
    std::vector<Failure>     failures;
    std::string              kind;     // the `begin` event's (P3.4); empty: unstated
    std::vector<std::string> tags;
    double                   t = -1;   // `begin`: milliseconds after the program started
    std::vector<Metric>      metrics;  // `metric` events (P6), in the order they came
    std::vector<Bound>       bounds;   // the `begin` event's (P6.5)
    std::optional<Checks>    checks;   // the `end` event's (O3)
    // SPEC-017 O8: what the case wrote, by the program's `output` events or,
    // for a program that ran this case alone, all that the program wrote.
    std::string              output;
    bool                     outputStated = false;   // an `output` event came
};

// What one program's report says.
struct Report {
    int                        level = 0;      // the `program` event's; 0 when none came
    // A `program` event came. A program that carries the marker and writes
    // none spoke no protocol (a file of plain `main` linked beside a
    // framework's library): it is one case, judged by its exit status.
    bool                       spoke = false;
    std::string                framework;
    bool                       done = false;   // the `done` event came
    std::vector<Case>          listed;         // `case` events (list mode)
    std::vector<CaseResult>    cases;          // in the order they began
    std::vector<std::string>   missing;        // P4.1
    Usage                      usage;          // the `usage` event (P10.2)
    // A case that began and did not end failed, with the program's exit
    // status as its reason (P5.4).
    bool failed() const;
    std::map<std::string, int> counts() const;
};

// The report a program appended to `file`; `exitStatus` is the program's, for
// the cases a crash left without an end (P5.4). Lines that are not JSON
// objects, and fields and events this version does not know, are ignored
// (P5.4).
Report read_report(const std::filesystem::path& file, int exitStatus);

// A REPORT READ AS THE PROGRAM APPENDS IT (P5.1): each `poll` takes the
// complete lines written since the last one and says which cases began and
// ended in them, so a runner can state a case's result while the program
// still runs. `finish` closes it as `read_report` does.
class ReportReader {
public:
    struct Change {
        std::size_t index;   // into `report().cases`
        bool        ended;   // false: the case began
    };
    std::vector<Change> poll(const std::filesystem::path& file);
    const Report& report() const { return r_; }
    Report finish(int exitStatus);
private:
    Report                             r_;
    std::map<std::string, std::size_t> open_;
    std::uintmax_t                     offset_ = 0;
};

// The variables a run states (P2.1): `list` or `run`, the report, the selection
// file when there is one, the directory the cases may use, and the seed.
std::vector<std::pair<std::string, std::string>>
variables(std::string_view mode, const std::filesystem::path& report,
          const std::optional<std::filesystem::path>& select,
          const std::filesystem::path& tmpdir, std::optional<std::uint64_t> seed);

// Whether a case identifier matches a pattern of `mcpp test --case`: `*` any
// run of characters, `?` one; a pattern without either matches the identifier
// it equals or the identifiers it is a `/`-prefix of (`fs` matches `fs/read`).
bool matches(std::string_view id, std::string_view pattern);

// The identifiers `mcpp test --case` passes a program (P4.1), from its listing:
// each listed case a pattern matches; for a pattern under a listed case -- a
// case that case declares, which the program reports when it runs and need not
// list (P3.4) -- the pattern itself when it names one case, else the listed
// case, all of whose cases then run.
std::vector<std::string> selection(const std::vector<Case>& listed,
                                   const std::vector<std::string>& patterns);

// The JSON of a report in a `mcpp test --message-format json` record
// (docs/50): `cases` and `missing`.
nlohmann::json cases_json(const Report& r);
// One case's result as `cases_json` writes it: id, status, duration, reason,
// failures, and kind, tags, metrics and bounds when the program stated them.
nlohmann::json case_json(const CaseResult& c);
nlohmann::json bounds_json(const std::vector<Bound>& b);
nlohmann::json metric_json(const Metric& m);
nlohmann::json usage_json(const Usage& u);
nlohmann::json listing_json(const Report& r);

}  // namespace mcpp::build::test_protocol

namespace mcpp::build::test_protocol {

int marker_version(const std::filesystem::path& program) {
    std::ifstream in(program, std::ios::binary);
    if (!in) return 0;
    static constexpr std::string_view marker = "mtest-protocol=";
    // Read in blocks, keeping the marker's length minus one and a version's
    // digits across a block boundary.
    constexpr std::size_t block = 1 << 20;
    std::string window;
    std::vector<char> buffer(block);
    int first = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        window.append(buffer.data(), got);
        for (std::size_t at = window.find(marker); at != std::string::npos;
             at = window.find(marker, at + 1)) {
            std::size_t end = at + marker.size();
            while (end < window.size() && std::isdigit(static_cast<unsigned char>(window[end])))
                ++end;
            if (end == window.size() && in) break;   // the version continues in the next block
            if (end == at + marker.size()) continue;
            int version = 0;
            std::from_chars(window.data() + at + marker.size(), window.data() + end, version);
            if (version == kVersion) return version;
            if (version > 0 && first == 0) first = version;
        }
        const std::size_t keep = marker.size() + 8;
        if (window.size() > keep) window.erase(0, window.size() - keep);
    }
    return first;
}

bool Report::failed() const {
    if (!missing.empty()) return true;
    return std::ranges::any_of(cases, [](const CaseResult& c) {
        return c.status == "fail" || c.status == "xpass";
    });
}

std::map<std::string, int> Report::counts() const {
    std::map<std::string, int> out;
    for (const auto& c : cases) ++out[c.status];
    return out;
}

namespace {

std::string text(const nlohmann::json& e, const char* key) {
    auto it = e.find(key);
    return it != e.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

int number(const nlohmann::json& e, const char* key) {
    auto it = e.find(key);
    return it != e.end() && it->is_number() ? it->get<int>() : 0;
}

std::vector<std::string> strings(const nlohmann::json& e, const char* key) {
    std::vector<std::string> out;
    auto it = e.find(key);
    if (it == e.end() || !it->is_array()) return out;
    for (const auto& v : *it)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

std::vector<Bound> bounds_of(const nlohmann::json& e) {
    std::vector<Bound> out;
    auto it = e.find("bounds");
    if (it == e.end() || !it->is_array()) return out;
    for (const auto& b : *it) {
        if (!b.is_object()) continue;
        Bound one{text(b, "metric"), {}, {}};
        if (auto m = b.find("max"); m != b.end() && m->is_number()) one.max = m->get<double>();
        if (auto m = b.find("min"); m != b.end() && m->is_number()) one.min = m->get<double>();
        out.push_back(std::move(one));
    }
    return out;
}

// The case `id` names, begun now when no `begin` came for it.
std::size_t case_of(Report& r, std::map<std::string, std::size_t>& open, const std::string& id) {
    if (auto it = open.find(id); it != open.end()) return it->second;
    r.cases.push_back({id, "", 0, "", {}});
    return open[id] = r.cases.size() - 1;
}

// One event into the report it belongs to; a case that began or ended is said.
std::optional<ReportReader::Change>
take(Report& r, std::map<std::string, std::size_t>& open, const nlohmann::json& e) {
    const auto kind = text(e, "e");
    const auto id = text(e, "id");
    if (kind == "program") {
        r.spoke = true;
        r.level = number(e, "level");
        r.framework = text(e, "framework");
    } else if (kind == "case") {
        r.listed.push_back({id, text(e, "name"), text(e, "file"), number(e, "line"),
                            text(e, "kind"), strings(e, "tags"), text(e, "expect"), bounds_of(e),
                            strings(e, "resources"),
                            e.contains("timeout") && e["timeout"].is_number() ? e["timeout"].get<double>() : 0.0});
    } else if (kind == "begin") {
        open[id] = r.cases.size();
        r.cases.push_back({id, "", 0, "", {}});
        auto& c = r.cases.back();
        c.kind = text(e, "kind");
        c.tags = strings(e, "tags");
        c.bounds = bounds_of(e);
        if (auto t = e.find("t"); t != e.end() && t->is_number()) c.t = t->get<double>();
        return ReportReader::Change{r.cases.size() - 1, false};
    } else if (kind == "failure") {
        Failure f{text(e, "file"), number(e, "line"), text(e, "message"), text(e, "expected"),
                  text(e, "actual")};
        f.source = text(e, "source");
        if (auto v = e.find("fatal"); v != e.end() && v->is_boolean()) f.fatal = v->get<bool>();
        r.cases[case_of(r, open, id)].failures.push_back(std::move(f));
    } else if (kind == "output") {
        auto& c = r.cases[case_of(r, open, id)];
        c.output += text(e, "text");
        c.outputStated = true;
    } else if (kind == "metric") {
        // A MEASUREMENT IS A CASE'S RESULT TOO (P6): kept with the case, so a
        // record and a store hold what `mtest analyze bench|trend` reads.
        Metric m{text(e, "name"), 0, text(e, "unit"), {}, text(e, "better"), text(e, "precision")};
        if (auto v = e.find("value"); v != e.end() && v->is_number()) m.value = v->get<double>();
        if (auto s = e.find("samples"); s != e.end() && s->is_array())
            for (const auto& x : *s)
                if (x.is_number()) m.samples.push_back(x.get<double>());
        r.cases[case_of(r, open, id)].metrics.push_back(std::move(m));
    } else if (kind == "usage") {
        r.usage.set = true;
        if (auto v = e.find("user_ms"); v != e.end() && v->is_number()) r.usage.userMs = v->get<double>();
        if (auto v = e.find("sys_ms"); v != e.end() && v->is_number()) r.usage.sysMs = v->get<double>();
        auto bytes = [&](const char* key, std::optional<std::uint64_t>& into) {
            if (auto v = e.find(key); v != e.end() && v->is_number()) into = v->get<std::uint64_t>();
        };
        bytes("peak_bytes", r.usage.peakBytes);
        bytes("read_bytes", r.usage.readBytes);
        bytes("written_bytes", r.usage.writtenBytes);
    } else if (kind == "end") {
        const auto i = case_of(r, open, id);
        auto& c = r.cases[i];
        c.status = text(e, "status");
        if (auto ms = e.find("ms"); ms != e.end() && ms->is_number()) c.ms = ms->get<double>();
        c.reason = text(e, "reason");
        if (auto k = e.find("checks"); k != e.end() && k->is_object()) {
            Checks n;
            if (auto v = k->find("held"); v != k->end() && v->is_number_integer()) n.held = v->get<long long>();
            if (auto v = k->find("failed"); v != k->end() && v->is_number_integer()) n.failed = v->get<long long>();
            c.checks = n;
        }
        return ReportReader::Change{i, true};
    } else if (kind == "missing") {
        r.missing.push_back(id);
    } else if (kind == "done") {
        r.done = true;
    }
    return std::nullopt;
}

}  // namespace

std::vector<ReportReader::Change> ReportReader::poll(const std::filesystem::path& file) {
    std::vector<Change> out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;
    in.seekg(static_cast<std::streamoff>(offset_));
    std::string text{std::istreambuf_iterator<char>(in), {}};
    // Only complete lines: a line the program is still writing is read with
    // the next poll.
    std::size_t at = 0;
    for (std::size_t nl = text.find('\n'); nl != std::string::npos; nl = text.find('\n', at)) {
        auto e = nlohmann::json::parse(std::string_view(text).substr(at, nl - at), nullptr, false);
        at = nl + 1;
        if (e.is_discarded() || !e.is_object()) continue;
        if (auto c = take(r_, open_, e)) out.push_back(*c);
    }
    offset_ += at;
    return out;
}


Report ReportReader::finish(int exitStatus) {
    for (auto& c : r_.cases)
        if (c.status.empty()) {
            c.status = "fail";
            c.reason = std::format("the program ended with status {} before the case ended",
                                   exitStatus);
        }
    return std::move(r_);
}

Report read_report(const std::filesystem::path& file, int exitStatus) {
    ReportReader reader;
    reader.poll(file);
    return reader.finish(exitStatus);
}

std::vector<std::pair<std::string, std::string>>
variables(std::string_view mode, const std::filesystem::path& report,
          const std::optional<std::filesystem::path>& select,
          const std::filesystem::path& tmpdir, std::optional<std::uint64_t> seed) {
    std::vector<std::pair<std::string, std::string>> env = {
        {"MTEST_PROTOCOL", std::to_string(kVersion)},
        {"MTEST_MODE", std::string(mode)},
        {"MTEST_REPORT", report.string()},
        {"MTEST_TMPDIR", tmpdir.string()},
    };
    if (select) env.emplace_back("MTEST_SELECT", select->string());
    if (seed) env.emplace_back("MTEST_SEED", std::to_string(*seed));
    return env;
}

namespace {

bool glob(std::string_view text, std::string_view pattern) {
    std::size_t t = 0, p = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++t;
            ++p;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

}  // namespace

bool matches(std::string_view id, std::string_view pattern) {
    if (pattern.find_first_of("*?") != std::string_view::npos) return glob(id, pattern);
    return id == pattern || (id.starts_with(pattern) && id.size() > pattern.size()
                             && id[pattern.size()] == '/');
}

std::vector<std::string> selection(const std::vector<Case>& listed,
                                   const std::vector<std::string>& patterns) {
    std::vector<std::string> out;
    auto add = [&](const std::string& id) {
        if (std::ranges::find(out, id) == out.end()) out.push_back(id);
    };
    for (const auto& p : patterns) {
        bool found = false;
        for (const auto& c : listed)
            if (matches(c.id, p)) { add(c.id); found = true; }
        if (found) continue;
        const auto under = std::ranges::find_if(listed, [&](const Case& c) {
            return p.size() > c.id.size() && p.starts_with(c.id) && p[c.id.size()] == '/';
        });
        if (under == listed.end()) continue;
        add(p.find_first_of("*?") == std::string::npos ? p : under->id);
    }
    return out;
}

nlohmann::json bounds_json(const std::vector<Bound>& b) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& one : b) {
        nlohmann::json j = {{"metric", one.metric}};
        if (one.max) j["max"] = *one.max;
        if (one.min) j["min"] = *one.min;
        out.push_back(std::move(j));
    }
    return out;
}

nlohmann::json metric_json(const Metric& m) {
    nlohmann::json j = {{"name", m.name}, {"value", m.value}, {"unit", m.unit}};
    if (!m.samples.empty()) j["samples"] = m.samples;
    if (!m.better.empty()) j["better"] = m.better;
    if (!m.precision.empty()) j["precision"] = m.precision;
    return j;
}

nlohmann::json usage_json(const Usage& u) {
    nlohmann::json j = {{"user_ms", u.userMs}, {"sys_ms", u.sysMs}};
    if (u.peakBytes) j["peak_bytes"] = *u.peakBytes;
    if (u.readBytes) j["read_bytes"] = *u.readBytes;
    if (u.writtenBytes) j["written_bytes"] = *u.writtenBytes;
    return j;
}

nlohmann::json case_json(const CaseResult& c) {
    nlohmann::json one = {{"id", c.id}, {"status", c.status}, {"duration_ms", c.ms}};
    if (!c.reason.empty()) one["reason"] = c.reason;
    if (!c.failures.empty()) {
        one["failures"] = nlohmann::json::array();
        for (const auto& f : c.failures) {
            nlohmann::json failure = {{"file", f.file}, {"line", f.line}, {"message", f.message}};
            if (!f.expected.empty() || !f.actual.empty()) {
                failure["expected"] = f.expected;
                failure["actual"] = f.actual;
            }
            if (!f.source.empty()) failure["source"] = f.source;
            if (f.fatal) failure["fatal"] = true;
            one["failures"].push_back(std::move(failure));
        }
    }
    if (!c.kind.empty()) one["kind"] = c.kind;
    if (!c.tags.empty()) one["tags"] = c.tags;
    if (!c.metrics.empty()) {
        one["metrics"] = nlohmann::json::array();
        for (const auto& m : c.metrics) one["metrics"].push_back(metric_json(m));
    }
    if (!c.bounds.empty()) one["bounds"] = bounds_json(c.bounds);
    if (c.checks) one["checks"] = {{"held", c.checks->held}, {"failed", c.checks->failed}};
    if (!c.output.empty()) one["output"] = c.output;
    return one;
}

nlohmann::json cases_json(const Report& r) {
    nlohmann::json cases = nlohmann::json::array();
    for (const auto& c : r.cases) cases.push_back(case_json(c));
    return cases;
}

nlohmann::json listing_json(const Report& r) {
    nlohmann::json cases = nlohmann::json::array();
    for (const auto& c : r.listed) {
        nlohmann::json one = {{"id", c.id}, {"name", c.name}, {"file", c.file}, {"line", c.line}};
        if (!c.kind.empty()) one["kind"] = c.kind;
        if (!c.tags.empty()) one["tags"] = c.tags;
        if (!c.expect.empty()) one["expect"] = c.expect;
        if (!c.bounds.empty()) one["bounds"] = bounds_json(c.bounds);
        cases.push_back(std::move(one));
    }
    return cases;
}

}  // namespace mcpp::build::test_protocol
