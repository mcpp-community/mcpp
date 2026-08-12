// The bench result protocol: what a measurement IS, independent of who produced
// it or what reads it.
//
// This module is the one piece of the suite that is expensive to change, so it
// is deliberately small and carries no logic beyond serialisation. Engines,
// scenarios and analysis all write toward these types; none of them may add a
// field without bumping kProtocolVersion.
//
// Three invariants are encoded here rather than left to convention, because each
// one was violated by the shell harness this suite replaces:
//
//   1. A FAILURE MUST NOT BE ABLE TO LOOK LIKE A MEASUREMENT. `status` and the
//      timings are separate fields, and a non-ok status carries no median. The
//      old harness formatted a failed cell as "0.000 s" and three of them went
//      into a results file looking like the fastest builds ever recorded.
//   2. A SKIP MUST CARRY ITS REASON. "bazel is not installed here" and "bazel
//      ran and failed" are opposite conclusions; `Unavailable` vs `Failed` plus
//      a mandatory note keeps them apart.
//   3. RESULTS TRAVEL WITH THEIR HOST. A wall-clock number without the machine
//      that produced it is not comparable to anything — least of all on a
//      heterogeneous CPU, where "32 cores" is not 32 of the same thing.
export module bench.protocol;

import std;

export namespace bench {

// Bump on ANY field addition/removal/semantic change. Readers compare against
// their own expectation and degrade explicitly rather than mis-parsing.
inline constexpr int kProtocolVersion = 1;

// ---------------------------------------------------------------------------

enum class Status { Ok, Failed, Skipped, Unavailable };

constexpr std::string_view to_string(Status s) {
    switch (s) {
        case Status::Ok:          return "ok";
        case Status::Failed:      return "failed";
        case Status::Skipped:     return "skipped";
        case Status::Unavailable: return "unavailable";
    }
    return "unknown";
}

// The source form the fixture is expressed in. This is the axis the whole suite
// exists to measure, so it is a first-class enum rather than a string tag.
enum class Variant {
    Headers,      // classic headers + separate .cpp implementation
    Modules,      // module interface units carrying their implementations
    ModulesImpl,  // module interface units + separate implementation units
};

constexpr std::string_view to_string(Variant v) {
    switch (v) {
        case Variant::Headers:     return "headers";
        case Variant::Modules:     return "modules";
        case Variant::ModulesImpl: return "modules-impl";
    }
    return "unknown";
}

constexpr std::optional<Variant> variant_from(std::string_view s) {
    if (s == "headers")      return Variant::Headers;
    if (s == "modules")      return Variant::Modules;
    if (s == "modules-impl") return Variant::ModulesImpl;
    return std::nullopt;
}

// ---------------------------------------------------------------------------

struct HostInfo {
    std::string   os;
    std::string   arch;
    std::string   cpu_model;
    int           logical_cores{};
    int           physical_cores{};
    // A 13900K is 8 P-cores + 16 E-cores. Reading its 32 threads as 32 equal
    // cores makes every parallelism figure wrong, so the fact is recorded
    // rather than inferred by whoever reads the numbers later.
    bool          heterogeneous{};
    std::uint64_t ram_bytes{};
    std::string   toolchain;   // e.g. "gcc 16.1.0"
};

// The full coordinate of one measurement. Every field is part of the identity;
// two cells differing in any of them are different measurements, never repeats.
struct CellKey {
    std::string engine;
    std::string compiler;
    std::string profile;
    std::string scenario;
    std::string fixture;
    std::string variant;

    [[nodiscard]] std::string str() const {
        return std::format("{}/{}/{}/{}/{}/{}", engine, compiler, profile, scenario,
                           fixture, variant);
    }
};

struct Sample {
    double wall_s{};
    int    exit_code{};
};

class CellResult {
public:
    CellKey             key;
    std::vector<Sample> samples;
    Status              status{Status::Skipped};
    std::string         note;      // required whenever status != Ok

    // Timings are DERIVED, never set alongside a non-ok status — that is what
    // makes invariant 1 structural instead of a review comment.
    [[nodiscard]] bool has_timing() const { return status == Status::Ok && !samples.empty(); }

    [[nodiscard]] double median_s() const {
        if (!has_timing()) return 0.0;
        std::vector<double> v;
        v.reserve(samples.size());
        for (const auto& s : samples) v.push_back(s.wall_s);
        std::ranges::sort(v);
        const auto n = v.size();
        return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
    }
    [[nodiscard]] double min_s() const {
        if (!has_timing()) return 0.0;
        return std::ranges::min(samples, {}, &Sample::wall_s).wall_s;
    }
    [[nodiscard]] double max_s() const {
        if (!has_timing()) return 0.0;
        return std::ranges::max(samples, {}, &Sample::wall_s).wall_s;
    }
};

struct Report {
    HostInfo                 host;
    std::string              started_at;   // ISO-8601, filled by the caller
    std::vector<CellResult>  cells;
};

// ---------------------------------------------------------------------------
// Serialisation. Hand-rolled on purpose: the suite must build with nothing but
// `import std;` so it can be the FIRST thing that runs on a fresh machine.
// ---------------------------------------------------------------------------

namespace detail {

inline std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                else
                    out += c;
        }
    }
    return out;
}

inline std::string q(std::string_view s) { return std::format("\"{}\"", escape(s)); }

// Fixed precision everywhere: a result file is diffed and merged across runs,
// and shortest-round-trip formatting makes those diffs noisy for no gain.
inline std::string num(double v) { return std::format("{:.3f}", v); }

}  // namespace detail

inline std::string to_json(const Report& r) {
    using detail::q;
    using detail::num;
    std::string out;
    out += "{\n";
    out += std::format("  \"protocol_version\": {},\n", kProtocolVersion);
    out += std::format("  \"started_at\": {},\n", q(r.started_at));
    out += "  \"host\": {\n";
    out += std::format("    \"os\": {},\n", q(r.host.os));
    out += std::format("    \"arch\": {},\n", q(r.host.arch));
    out += std::format("    \"cpu_model\": {},\n", q(r.host.cpu_model));
    out += std::format("    \"logical_cores\": {},\n", r.host.logical_cores);
    out += std::format("    \"physical_cores\": {},\n", r.host.physical_cores);
    out += std::format("    \"heterogeneous\": {},\n", r.host.heterogeneous ? "true" : "false");
    out += std::format("    \"ram_bytes\": {},\n", r.host.ram_bytes);
    out += std::format("    \"toolchain\": {}\n", q(r.host.toolchain));
    out += "  },\n";
    out += "  \"cells\": [\n";
    for (std::size_t i = 0; i < r.cells.size(); ++i) {
        const auto& c = r.cells[i];
        out += "    {\n";
        out += std::format("      \"engine\": {},\n",   q(c.key.engine));
        out += std::format("      \"compiler\": {},\n", q(c.key.compiler));
        out += std::format("      \"profile\": {},\n",  q(c.key.profile));
        out += std::format("      \"scenario\": {},\n", q(c.key.scenario));
        out += std::format("      \"fixture\": {},\n",  q(c.key.fixture));
        out += std::format("      \"variant\": {},\n",  q(c.key.variant));
        out += std::format("      \"status\": {},\n",   q(to_string(c.status)));
        out += std::format("      \"note\": {},\n",     q(c.note));
        out += std::format("      \"runs\": {},\n",     c.samples.size());
        if (c.has_timing()) {
            out += std::format("      \"median_s\": {},\n", num(c.median_s()));
            out += std::format("      \"min_s\": {},\n",    num(c.min_s()));
            out += std::format("      \"max_s\": {},\n",    num(c.max_s()));
            out += "      \"samples\": [";
            for (std::size_t k = 0; k < c.samples.size(); ++k)
                out += std::format("{}{}", k ? ", " : "", num(c.samples[k].wall_s));
            out += "]\n";
        } else {
            // No timing keys at all rather than zeros: a reader that forgets to
            // check `status` gets a missing key (loud) instead of a 0.0 (silent).
            out += "      \"samples\": []\n";
        }
        out += (i + 1 == r.cells.size()) ? "    }\n" : "    },\n";
    }
    out += "  ]\n";
    out += "}\n";
    return out;
}

}  // namespace bench
