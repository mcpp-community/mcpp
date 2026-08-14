// bench.journal — implementation.
//
// `module bench.journal;` with no `export`: this is an implementation unit, so
// nothing here reaches the BMI. See journal.cppm for why that matters.
module bench.journal;

import std;
import bench.protocol;

namespace bench {
namespace {

// Minimal JSON string escape, private to this unit.
std::string esc(std::string_view v) {
    std::string out;
    for (const char c : v) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

}  // namespace

Journal::Journal(std::filesystem::path path) : path_(std::move(path)) {}

const std::filesystem::path& Journal::path() const { return path_; }

std::string Journal::unit_id(std::string_view project, std::string_view variant,
                             std::string_view scenario, std::string_view engine, int run) {
    return std::format("{}|{}|{}|{}|{}", project, variant, scenario, engine, run);
}

void Journal::append(const JournalEntry& e) const {
    std::ofstream out(path_, std::ios::app);
    if (!out) return;
    out << std::format(
        R"({{"id":"{}","ver":"{}","project":"{}","variant":"{}","scenario":"{}",)"
        R"("engine":"{}","run":{},"wall_s":{:.6f},"exit":{}}})",
        esc(e.id), esc(e.engine_version), esc(e.project), esc(e.variant), esc(e.scenario),
        esc(e.engine), e.run, e.wall_s, e.exit_code)
        << '\n';
    out.flush();
}

Journal::Loaded Journal::load(std::string_view want_id) const {
    Loaded out;
    std::ifstream in(path_);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        // A line that is not a whole JSON object is skipped, not fatal: the last
        // line of a killed run is expected to be half-written.
        if (line.empty() || line.front() != '{' || line.back() != '}') {
            if (!line.empty()) ++out.skipped_unparsable;
            continue;
        }
        const auto get = [&](std::string_view k) -> std::string {
            const auto at = line.find(std::format("\"{}\":", k));
            if (at == std::string::npos) return {};
            const auto v = line.find_first_not_of(' ', at + k.size() + 3);
            if (v == std::string::npos) return {};
            if (line[v] == '"') {
                const auto e = line.find('"', v + 1);
                return e == std::string::npos ? std::string{} : line.substr(v + 1, e - v - 1);
            }
            const auto e = line.find_first_of(",}", v);
            return line.substr(v, e - v);
        };
        JournalEntry e;
        e.id = get("id");
        if (e.id != want_id) {
            ++out.skipped_other_id;
            if (out.other_id.empty()) out.other_id = e.id;
            continue;
        }
        e.engine_version = get("ver");
        e.project        = get("project");
        e.variant        = get("variant");
        e.scenario       = get("scenario");
        e.engine         = get("engine");
        const auto run_s  = get("run");
        const auto wall_s = get("wall_s");
        const auto exit_s = get("exit");
        if (run_s.empty() || wall_s.empty()) { ++out.skipped_unparsable; continue; }
        e.run       = std::atoi(run_s.c_str());
        e.wall_s    = std::atof(wall_s.c_str());
        e.exit_code = exit_s.empty() ? 0 : std::atoi(exit_s.c_str());
        out.units[unit_id(e.project, e.variant, e.scenario, e.engine, e.run)] = e;
    }
    return out;
}

}  // namespace bench
