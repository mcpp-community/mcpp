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
inline constexpr std::string_view kMtestRelease = "0.3.0";
inline constexpr std::string_view kMtestRepo    = "https://github.com/mcpp-community/mtest";

// The protocol's variables, removed from the environment a program inherits
// so that an outer runner's do not reach a program mcpp starts (P2.2).
inline constexpr std::array<std::string_view, 7> kVariables = {
    "MTEST_PROTOCOL", "MTEST_MODE", "MTEST_REPORT", "MTEST_SELECT",
    "MTEST_ISOLATION", "MTEST_TMPDIR", "MTEST_SEED",
};

// The version the marker in a program's file states (P2.3): the bytes
// `mtest-protocol=` and a version followed by a byte that is not a digit; 0
// when the file carries none or cannot be read.
int marker_version(const std::filesystem::path& program);

// What a program's case is, as its listing states it (P3.3).
struct Case {
    std::string              id;
    std::string              name;
    std::string              file;
    int                      line = 0;
    std::string              kind;
    std::vector<std::string> tags;
    std::string              expect;
};

// A failure an expectation of a case reported (P5.2).
struct Failure {
    std::string file;
    int         line = 0;
    std::string message;
    std::string expected;
    std::string actual;
};

// One case's result (P5.3), with its failures.
struct CaseResult {
    std::string          id;
    std::string          status;   // pass, fail, skip, xfail, xpass, invalid
    double               ms = 0;
    std::string          reason;
    std::vector<Failure> failures;
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
            if (version > 0) return version;
        }
        const std::size_t keep = marker.size() + 8;
        if (window.size() > keep) window.erase(0, window.size() - keep);
    }
    return 0;
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

// One event into the report it belongs to.
void take(Report& r, std::map<std::string, std::size_t>& open, const nlohmann::json& e) {
    const auto kind = text(e, "e");
    const auto id = text(e, "id");
    if (kind == "program") {
        r.spoke = true;
        r.level = number(e, "level");
        r.framework = text(e, "framework");
    } else if (kind == "case") {
        r.listed.push_back({id, text(e, "name"), text(e, "file"), number(e, "line"),
                            text(e, "kind"), strings(e, "tags"), text(e, "expect")});
    } else if (kind == "begin") {
        open[id] = r.cases.size();
        r.cases.push_back({id, "", 0, "", {}});
    } else if (kind == "failure") {
        auto it = open.find(id);
        if (it == open.end()) {
            open[id] = r.cases.size();
            r.cases.push_back({id, "", 0, "", {}});
            it = open.find(id);
        }
        r.cases[it->second].failures.push_back({text(e, "file"), number(e, "line"),
                                               text(e, "message"), text(e, "expected"),
                                               text(e, "actual")});
    } else if (kind == "end") {
        auto it = open.find(id);
        if (it == open.end()) {
            r.cases.push_back({id, "", 0, "", {}});
            it = open.emplace(id, r.cases.size() - 1).first;
        }
        auto& c = r.cases[it->second];
        c.status = text(e, "status");
        if (auto ms = e.find("ms"); ms != e.end() && ms->is_number()) c.ms = ms->get<double>();
        c.reason = text(e, "reason");
    } else if (kind == "missing") {
        r.missing.push_back(id);
    } else if (kind == "done") {
        r.done = true;
    }
}

}  // namespace

Report read_report(const std::filesystem::path& file, int exitStatus) {
    Report r;
    std::ifstream in(file, std::ios::binary);
    std::map<std::string, std::size_t> open;
    for (std::string line; in && std::getline(in, line);) {
        auto e = nlohmann::json::parse(line, nullptr, false);
        if (e.is_discarded() || !e.is_object()) continue;
        take(r, open, e);
    }
    for (auto& c : r.cases)
        if (c.status.empty()) {
            c.status = "fail";
            c.reason = std::format("the program ended with status {} before the case ended",
                                   exitStatus);
        }
    return r;
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

nlohmann::json cases_json(const Report& r) {
    nlohmann::json cases = nlohmann::json::array();
    for (const auto& c : r.cases) {
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
                one["failures"].push_back(std::move(failure));
            }
        }
        cases.push_back(std::move(one));
    }
    return cases;
}

nlohmann::json listing_json(const Report& r) {
    nlohmann::json cases = nlohmann::json::array();
    for (const auto& c : r.listed) {
        nlohmann::json one = {{"id", c.id}, {"name", c.name}, {"file", c.file}, {"line", c.line}};
        if (!c.kind.empty()) one["kind"] = c.kind;
        if (!c.tags.empty()) one["tags"] = c.tags;
        if (!c.expect.empty()) one["expect"] = c.expect;
        cases.push_back(std::move(one));
    }
    return cases;
}

}  // namespace mcpp::build::test_protocol
