// mcpp.build.record -- the record of a build (SPEC-017 B4 `builds.ndjson`, design 2026-11-20
// §5.6): an edge per line, with what ninja's log says of it (its outputs, start and end) and what
// the compiler executor measured (processor time, largest resident set), and a line per package
// the cache served. Three questions a build could not answer before (the long tail of a test
// build: which edges, how long each, on how many processors) are each a query of it.
export module mcpp.build.record;

import std;
import nlohmann.json;

export namespace mcpp::build::record {

// The variable that names the file the compiler executor appends its lines to: the build's
// reader sets it for ninja (`environment()` of a build's progress).
inline constexpr std::string_view kUsageEnv  = "MCPP_EDGE_USAGE";
inline constexpr std::string_view kUsageFile = ".mcpp-edge-usage";
// The variable that names a file each build of an invocation appends its record to: a runner
// that starts mcpp (a test kit's step) reads the build's identifier and edges from it.
inline constexpr std::string_view kRecordEnv = "MCPP_BUILD_RECORD";

// What the executor measured of one compile: its output as the edge names it, its start and end
// (milliseconds since the epoch), and the compiler's processor time and largest resident set,
// absent where the system does not tell them.
struct Usage {
    std::string                  output;
    long long                    startedUnixMs = 0;
    long long                    endedUnixMs = 0;
    std::optional<double>        cpuMs;
    std::optional<std::uint64_t> peakBytes;
};
// One line, `\n` ended, written in one append so that concurrent compiles do not interleave.
std::string usage_line(const Usage& u);
// The complete lines of `text`; a line that is not a usage line is skipped.
std::vector<Usage> parse_usage(std::string_view text);

// An edge that ran: what ninja's log and the plan say of it, and what the executor measured.
struct Edge {
    std::string                  output;     // its first output, as build.ninja spells it
    std::string                  package;    // the package the plan attributes it to; empty: the build's own
    std::string                  program;    // the test program it serves; empty when none or not known
    double                       startedMs = 0;   // after the build began
    double                       ms = 0;
    std::optional<double>        cpuMs;
    std::optional<std::uint64_t> peakBytes;
};
// A package the cache served, with the units it placed.
struct Cached {
    std::string package;
    std::size_t units = 0;
};
struct Record {
    std::string         id;      // unique for an invocation's build
    std::vector<Edge>   edges;
    std::vector<Cached> cached;
};

// A fresh identifier: the time of day and a random part.
std::string new_id();
// Each edge's measurement from `usage`, matched by its output; the newest line of an output wins.
void attach(Record& r, const std::vector<Usage>& usage);
// The record as `builds.ndjson` lines (SPEC-017 B4).
std::string to_ndjson(const Record& r);
// The records the lines of `text` hold, by identifier in the order first seen.
std::vector<Record> from_ndjson(std::string_view text);
// The `n` edges that took longest, longest first.
std::vector<Edge> slowest(const Record& r, std::size_t n);
// Their sums: edge time and processor time (where measured).
struct Totals {
    double      ms = 0;
    double      cpuMs = 0;
    std::size_t edges = 0, measured = 0, cachedUnits = 0;
};
Totals totals(const Record& r);

// The records of the builds this invocation ran, in the order they ended: what `mcpp test` keeps
// in its run (SPEC-017 B4). Safe from several threads.
void keep(Record r);
std::vector<Record> kept();

}  // namespace mcpp::build::record

namespace mcpp::build::record {

namespace {
using json = nlohmann::json;

std::string text_of(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
std::optional<double> number_of(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return std::nullopt;
    return it->get<double>();
}
std::optional<std::uint64_t> bytes_of(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return std::nullopt;
    return it->get<std::uint64_t>();
}
// Each complete line of `text` that is a JSON object.
std::vector<json> objects(std::string_view text) {
    std::vector<json> out;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        if (nl == std::string_view::npos) break;
        auto line = text.substr(0, nl);
        text.remove_prefix(nl + 1);
        auto j = json::parse(line, nullptr, false);
        if (j.is_object()) out.push_back(std::move(j));
    }
    return out;
}
}  // namespace

std::string usage_line(const Usage& u) {
    json j = {{"output", u.output}, {"started", u.startedUnixMs}, {"ended", u.endedUnixMs}};
    if (u.cpuMs) j["cpu_ms"] = *u.cpuMs;
    if (u.peakBytes) j["peak_bytes"] = *u.peakBytes;
    return j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
}

std::vector<Usage> parse_usage(std::string_view text) {
    std::vector<Usage> out;
    for (const auto& j : objects(text)) {
        Usage u{text_of(j, "output")};
        if (u.output.empty()) continue;
        u.startedUnixMs = static_cast<long long>(number_of(j, "started").value_or(0));
        u.endedUnixMs = static_cast<long long>(number_of(j, "ended").value_or(0));
        u.cpuMs = number_of(j, "cpu_ms");
        u.peakBytes = bytes_of(j, "peak_bytes");
        out.push_back(std::move(u));
    }
    return out;
}

std::string new_id() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y%m%dT%H%M%S}-{:06x}", now, std::random_device{}() & 0xffffff);
}

void attach(Record& r, const std::vector<Usage>& usage) {
    std::unordered_map<std::string_view, const Usage*> byOutput;
    for (const auto& u : usage) byOutput[u.output] = &u;
    for (auto& e : r.edges)
        if (auto it = byOutput.find(e.output); it != byOutput.end()) {
            e.cpuMs = it->second->cpuMs;
            e.peakBytes = it->second->peakBytes;
        }
}

std::string to_ndjson(const Record& r) {
    std::string out;
    for (const auto& e : r.edges) {
        json j = {{"build", r.id},       {"output", e.output},       {"package", e.package},
                  {"program", e.program}, {"started_ms", e.startedMs}, {"ms", e.ms}};
        if (e.cpuMs) j["cpu_ms"] = *e.cpuMs;
        if (e.peakBytes) j["peak_bytes"] = *e.peakBytes;
        out += j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    }
    for (const auto& c : r.cached)
        out += json{{"build", r.id}, {"package", c.package}, {"cached", true}, {"units", c.units}}
                   .dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    return out;
}

std::vector<Record> from_ndjson(std::string_view text) {
    std::vector<Record> out;
    for (const auto& j : objects(text)) {
        const auto id = text_of(j, "build");
        auto at = std::ranges::find(out, id, &Record::id);
        if (at == out.end()) at = out.insert(out.end(), Record{id});
        if (auto c = j.find("cached"); c != j.end() && c->is_boolean() && c->get<bool>()) {
            at->cached.push_back({text_of(j, "package"),
                                  static_cast<std::size_t>(number_of(j, "units").value_or(0))});
            continue;
        }
        at->edges.push_back({text_of(j, "output"), text_of(j, "package"), text_of(j, "program"),
                             number_of(j, "started_ms").value_or(0), number_of(j, "ms").value_or(0),
                             number_of(j, "cpu_ms"), bytes_of(j, "peak_bytes")});
    }
    return out;
}

std::vector<Edge> slowest(const Record& r, std::size_t n) {
    auto edges = r.edges;
    std::ranges::stable_sort(edges, std::greater{}, &Edge::ms);
    if (edges.size() > n) edges.resize(n);
    return edges;
}

Totals totals(const Record& r) {
    Totals t;
    for (const auto& e : r.edges) {
        t.ms += e.ms;
        ++t.edges;
        if (e.cpuMs) {
            t.cpuMs += *e.cpuMs;
            ++t.measured;
        }
    }
    for (const auto& c : r.cached) t.cachedUnits += c.units;
    return t;
}

namespace {
struct Kept {
    std::mutex          m;
    std::vector<Record> all;
};
Kept& kept_records() {
    static Kept k;
    return k;
}
}  // namespace

void keep(Record r) {
    auto& k = kept_records();
    std::lock_guard lock(k.m);
    k.all.push_back(std::move(r));
}

std::vector<Record> kept() {
    auto& k = kept_records();
    std::lock_guard lock(k.m);
    return k.all;
}

}  // namespace mcpp::build::record
