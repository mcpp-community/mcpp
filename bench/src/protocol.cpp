// bench.protocol — implementation.
//
// `module bench.protocol;` with no `export`: an implementation unit. Nothing
// written here lands in the BMI, which is the point — protocol is imported by
// every other module in the suite, so it has the largest blast radius of any
// interface here. Under GCC 16.1 an exported definition mentioning std types
// can corrupt the BMIs of importers and report the failure against an unrelated
// module (this suite lost an afternoon to exactly that; see journal.cppm).
//
// What stays in the interface: enums, aggregates, and the `constexpr` mappings
// over them — those must be usable at compile time, and none of them carries a
// std container.
module bench.protocol;

import std;

namespace bench {

std::string CellKey::str() const {
    return std::format("{}/{}/{}/{}/{}/{}", engine, compiler, profile, scenario, fixture, variant);
}

bool CellResult::has_timing() const { return status == Status::Ok && !samples.empty(); }

double CellResult::median_s() const {
    if (!has_timing()) return 0.0;
    std::vector<double> v;
    v.reserve(samples.size());
    for (const auto& s : samples) v.push_back(s.wall_s);
    std::ranges::sort(v);
    const auto n = v.size();
    return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

double CellResult::min_s() const {
    if (!has_timing()) return 0.0;
    return std::ranges::min(samples, {}, &Sample::wall_s).wall_s;
}

double CellResult::max_s() const {
    if (!has_timing()) return 0.0;
    return std::ranges::max(samples, {}, &Sample::wall_s).wall_s;
}

std::string RunId::str() const { return fingerprint; }

RunId RunId::of(std::string config_text) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : config_text) { h ^= c; h *= 1099511628211ULL; }
    return RunId{std::format("{:08x}", static_cast<std::uint32_t>(h ^ (h >> 32))),
                 std::move(config_text)};
}

namespace detail {

std::string escape(std::string_view s) {
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

std::string q(std::string_view s) { return std::format("\"{}\"", escape(s)); }

// Fixed precision everywhere: a result file is diffed and merged across runs,
// and shortest-round-trip formatting makes those diffs noisy for no gain.
std::string num(double v) { return std::format("{:.3f}", v); }

}  // namespace detail


std::string to_json(const Report& r) {
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
