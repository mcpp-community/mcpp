// mcpp.build.result — what a build left behind, for whoever was not watching
// (design 2026-10-26 §5.1–§5.3, D2).
//
// THE RECORD. A build of one configuration writes `result.json` in its build
// directory twice: when it starts writing the directory (`"status":
// "running"`, with its pid) and when it ends (`ok`, `failed`). A build ended by
// SIGINT, SIGTERM or SIGHUP (Windows: a console control event) leaves
// `cancelled`: the interrupted form is rendered when the run starts and the
// handler writes it with async-signal-safe calls. SIGKILL runs no handler, so a
// record still `running` whose process is gone is a build that was killed. `target/.last-result.json`
// names the configuration the most recent build of the project wrote. Both are
// replaced atomically. The record is how an IDE learns what a build in a
// terminal did, and how an agent learns what the IDE's build did: the chains
// meet at mcpp, so mcpp writes the record (D1, D2).
//
// THE EVENT STREAM. `mcpp build --message-format json` writes NDJSON on
// standard output while it works -- `plan`, `waiting`, `progress`,
// `diagnostic`, `artifact` -- and ends with the `mcpp.build` envelope, whose
// `data` is this record. The narration stays on standard error, as for every
// machine-readable mode (docs/50 §3).
//
// THE LAUNCH DESCRIPTOR. A runnable artifact carries how `mcpp run` would
// start it: `argv`, the environment mcpp adds (only what it injects or
// rewrites -- the caller's own environment is not copied into target/), and
// `cwd`. An artifact that a runner or an execution host wraps says so
// (`via`), and `mcpp run --print-launch` answers for it.
export module mcpp.build.result;

import std;
import mcpp.libs.json;
import mcpp.version;                 // MCPP_VERSION
import mcpp.run;
import mcpp.ui.terminal;
import mcpp.invocation;

export namespace mcpp::build::result {

struct Launch {
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> env;
    std::string cwd;
    std::string via;   // "" | "runner" | "run_under"
};

// Where a program begins and where its image is entered (design 2026-10-26
// §24.4 item 10): `from` is `<package>/<entry>` when a dependency's entry
// target is the beginning, `start` the stated entry symbol and `startFrom`
// the statement that named it. Absent from the record when all are empty.
struct Entry {
    std::string from;
    std::string start;
    std::string startFrom;
};

struct Artifact {
    std::string name;
    std::string kind;   // bin | test | shared | static
    std::filesystem::path path;
    std::optional<Launch> launch;
    std::optional<Entry>  entry;
};

// A compiler's diagnostic, from the output of a failed step.
struct Diagnostic {
    std::string severity;   // error | warning | note
    std::string message;
    std::string path;
    int line = 0;
    int column = 0;
};

struct Record {
    std::string command;
    std::string client;
    long        pid = 0;
    std::int64_t startedAt = 0;    // ms since the epoch
    std::int64_t finishedAt = 0;
    std::string status;            // running | ok | failed | cancelled
    std::string triple;
    std::string profile;
    std::string fingerprint;
    std::filesystem::path outputDir;
    std::filesystem::path projectRoot;
    std::filesystem::path compileDatabase;
    std::vector<Artifact> artifacts;
    std::vector<Diagnostic> diagnostics;
};

nlohmann::json to_json(const Launch& l);
nlohmann::json to_json(const Record& r);
std::optional<Record> read(const std::filesystem::path& outputDir);

// Writes `<outputDir>/result.json` and `<projectRoot>/target/.last-result.json`.
void write(const Record& r);

// A record for this process, started now.
Record begin(const std::filesystem::path& projectRoot, const std::filesystem::path& outputDir);

std::int64_t now_ms();

// GCC, Clang and MSVC diagnostic lines in `text`.
std::vector<Diagnostic> parse_compiler_diagnostics(std::string_view text);

// ── the event stream ──
void enable_events(bool on);
bool events_enabled();
// One NDJSON line on stdout: {"schemaVersion":1,"kind":"mcpp.event","event":<name>, ...fields}.
void emit(std::string_view event, nlohmann::json fields = nlohmann::json::object());

}  // namespace mcpp::build::result

namespace mcpp::build::result {

namespace {
std::atomic<bool> g_events{false};
std::mutex g_emit;

void write_atomically(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += std::format(".{:x}", std::random_device{}());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << text;
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}
}  // namespace

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

nlohmann::json to_json(const Launch& l) {
    nlohmann::json env = nlohmann::json::object();
    for (auto const& [k, v] : l.env) env[k] = v;
    nlohmann::json j{{"argv", l.argv}, {"env", std::move(env)}, {"cwd", l.cwd}};
    j["via"] = l.via.empty() ? nlohmann::json(nullptr) : nlohmann::json(l.via);
    return j;
}

nlohmann::json to_json(const Record& r) {
    nlohmann::json artifacts = nlohmann::json::array();
    for (auto const& a : r.artifacts) {
        nlohmann::json j{{"name", a.name}, {"kind", a.kind}, {"path", a.path.generic_string()}};
        if (a.launch) j["launch"] = to_json(*a.launch);
        if (a.entry) {
            nlohmann::json e = nlohmann::json::object();
            if (!a.entry->from.empty()) e["from"] = a.entry->from;
            if (!a.entry->start.empty()) {
                e["start"] = a.entry->start;
                e["start_from"] = a.entry->startFrom;
            }
            j["entry"] = std::move(e);
        }
        artifacts.push_back(std::move(j));
    }
    nlohmann::json diags = nlohmann::json::array();
    for (auto const& d : r.diagnostics) {
        nlohmann::json j{{"severity", d.severity}, {"message", d.message}};
        if (!d.path.empty()) {
            j["path"] = d.path;
            if (d.line > 0)
                j["range"] = {{"start", {{"line", d.line}, {"column", std::max(d.column, 1)}}},
                              {"end", {{"line", d.line}, {"column", std::max(d.column, 1)}}}};
        }
        diags.push_back(std::move(j));
    }
    return nlohmann::json{
        {"command", r.command},
        {"client", r.client},
        {"pid", r.pid},
        {"startedAt", r.startedAt},
        {"finishedAt", r.finishedAt == 0 ? nlohmann::json(nullptr) : nlohmann::json(r.finishedAt)},
        {"status", r.status},
        {"configuration", {
            {"triple", r.triple},
            {"profile", r.profile},
            {"fingerprint", r.fingerprint},
            {"outputDir", r.outputDir.generic_string()},
        }},
        {"compileDatabase", r.compileDatabase.empty() ? nlohmann::json(nullptr)
                                                      : nlohmann::json(r.compileDatabase.generic_string())},
        {"artifacts", std::move(artifacts)},
        {"diagnostics", std::move(diags)},
    };
}

std::optional<Record> read(const std::filesystem::path& outputDir) {
    std::ifstream in(outputDir / "result.json", std::ios::binary);
    if (!in) return std::nullopt;
    auto doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.contains("data") || !doc["data"].is_object()) return std::nullopt;
    const auto& d = doc["data"];
    Record r;
    r.command = d.value("command", "");
    r.client = d.value("client", "");
    r.pid = d.value("pid", 0L);
    r.startedAt = d.value("startedAt", std::int64_t{0});
    r.status = d.value("status", "");
    if (d.contains("configuration") && d["configuration"].is_object()) {
        const auto& c = d["configuration"];
        r.triple = c.value("triple", "");
        r.profile = c.value("profile", "");
        r.fingerprint = c.value("fingerprint", "");
        r.outputDir = c.value("outputDir", "");
    }
    if (d.contains("compileDatabase") && d["compileDatabase"].is_string())
        r.compileDatabase = d["compileDatabase"].get<std::string>();
    if (d.contains("artifacts") && d["artifacts"].is_array())
        for (auto const& a : d["artifacts"]) {
            Artifact art{a.value("name", ""), a.value("kind", ""), a.value("path", ""),
                         std::nullopt, std::nullopt};
            if (a.contains("entry") && a["entry"].is_object()) {
                const auto& e = a["entry"];
                art.entry = Entry{e.value("from", ""), e.value("start", ""), e.value("start_from", "")};
            }
            if (a.contains("launch") && a["launch"].is_object()) {
                const auto& l = a["launch"];
                Launch launch;
                if (l.contains("argv") && l["argv"].is_array())
                    for (auto const& v : l["argv"]) if (v.is_string()) launch.argv.push_back(v.get<std::string>());
                if (l.contains("env") && l["env"].is_object())
                    for (auto it = l["env"].begin(); it != l["env"].end(); ++it)
                        if (it->is_string()) launch.env.emplace_back(it.key(), it->get<std::string>());
                launch.cwd = l.value("cwd", "");
                if (l.contains("via") && l["via"].is_string()) launch.via = l["via"].get<std::string>();
                art.launch = std::move(launch);
            }
            r.artifacts.push_back(std::move(art));
        }
    return r;
}

namespace {
std::string render(const Record& r) {
    nlohmann::json doc{
        {"schemaVersion", 1},
        {"kind", "mcpp.result"},
        {"kindVersion", 1},
        {"mcpp", {{"version", std::string(mcpp::MCPP_VERSION)}}},
        {"data", to_json(r)},
    };
    return doc.dump(2) + "\n";
}
std::string render_last(const Record& r) {
    nlohmann::json last{
        {"schemaVersion", 1},
        {"kind", "mcpp.last-result"},
        {"kindVersion", 1},
        {"data", {{"result", (r.outputDir / "result.json").generic_string()},
                  {"status", r.status},
                  {"finishedAt", r.finishedAt == 0 ? nlohmann::json(nullptr)
                                                   : nlohmann::json(r.finishedAt)}}},
    };
    return last.dump(2) + "\n";
}
}  // namespace

void write(const Record& r) {
    write_atomically(r.outputDir / "result.json", render(r));
    const auto last = r.projectRoot / "target" / ".last-result.json";
    if (!r.projectRoot.empty()) write_atomically(last, render_last(r));
    // A running record is armed with its interrupted form: a signal that ends
    // the build leaves `cancelled`, not a `running` record whose process is
    // gone (§5.4). The end of the build disarms it.
    if (r.status == "running") {
        auto cancelled = r;
        cancelled.status = "cancelled";
        std::vector<std::pair<std::filesystem::path, std::string>> files{
            {r.outputDir / "result.json", render(cancelled)}};
        if (!r.projectRoot.empty()) files.emplace_back(last, render_last(cancelled));
        mcpp::run::set_interrupt_files(files);
    } else {
        mcpp::run::clear_interrupt_files();
    }
}

std::vector<Diagnostic> parse_compiler_diagnostics(std::string_view text) {
    std::vector<Diagnostic> out;
    const auto severity_at = [](std::string_view s, std::size_t& len) -> std::string {
        for (std::string_view sev : {"fatal error", "error", "warning", "note"})
            if (s.starts_with(sev)) {
                len = sev.size();
                return std::string(sev == "fatal error" ? "error" : sev);
            }
        return {};
    };
    const auto number = [](std::string_view s, int& v) {
        return std::from_chars(s.data(), s.data() + s.size(), v).ec == std::errc{} && v > 0;
    };
    // GCC / Clang: <path>:<line>:<col>: <severity>: <message>, or without the
    // column, or with a program's name for the path (`cc1plus: error: …`).
    // A Windows drive letter's colon belongs to the path.
    const auto gnu = [&](std::string_view line) -> std::optional<Diagnostic> {
        for (auto at = line.find(": "); at != std::string_view::npos; at = line.find(": ", at + 2)) {
            std::size_t len = 0;
            auto severity = severity_at(line.substr(at + 2), len);
            if (severity.empty() || line.substr(at + 2 + len).substr(0, 2) != ": ") continue;
            auto loc = line.substr(0, at);
            Diagnostic d;
            d.severity = std::move(severity);
            d.message = std::string(line.substr(at + 4 + len));
            int a = 0, b = 0;
            auto c2 = loc.rfind(':');
            if (c2 != std::string_view::npos && c2 > 1 && number(loc.substr(c2 + 1), a)) {
                auto head = loc.substr(0, c2);
                auto c1 = head.rfind(':');
                if (c1 != std::string_view::npos && c1 > 1 && number(head.substr(c1 + 1), b)) {
                    d.path = std::string(head.substr(0, c1));
                    d.line = b;
                    d.column = a;
                } else {
                    d.path = std::string(head);
                    d.line = a;
                }
            } else if (loc.find(' ') == std::string_view::npos) {
                d.path = std::string(loc);
            } else {
                continue;
            }
            return d;
        }
        return std::nullopt;
    };
    // MSVC: <path>(<line>[,<col>]): <severity> <code>: <message>
    const auto msvc = [&](std::string_view line) -> std::optional<Diagnostic> {
        auto paren = line.find("): ");
        if (paren == std::string_view::npos) return std::nullopt;
        auto open = line.rfind('(', paren);
        if (open == std::string_view::npos) return std::nullopt;
        auto rest = line.substr(paren + 3);
        std::size_t len = 0;
        auto severity = severity_at(rest, len);
        if (severity.empty() || rest.substr(len).substr(0, 1) != " ") return std::nullopt;
        auto codeEnd = rest.find(": ", len);
        if (codeEnd == std::string_view::npos) return std::nullopt;
        Diagnostic d;
        d.severity = std::move(severity);
        d.message = std::string(rest.substr(codeEnd + 2));
        d.path = std::string(line.substr(0, open));
        auto nums = line.substr(open + 1, paren - open - 1);
        auto comma = nums.find(',');
        if (!number(nums.substr(0, comma), d.line)) return std::nullopt;
        if (comma != std::string_view::npos) number(nums.substr(comma + 1), d.column);
        return d;
    };
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (auto d = msvc(line)) out.push_back(std::move(*d));
        else if (auto g = gnu(line)) out.push_back(std::move(*g));
    }
    return out;
}

Record begin(const std::filesystem::path& projectRoot, const std::filesystem::path& outputDir) {
    Record r;
    r.startedAt = now_ms();
    r.status = "running";
    r.outputDir = outputDir;
    r.projectRoot = projectRoot;
    r.fingerprint = outputDir.filename().string();
    if (auto c = mcpp::invocation::get("MCPP_CLIENT")) r.client = *c;
    return r;
}

void enable_events(bool on) { g_events.store(on); }
bool events_enabled() { return g_events.load(); }

void emit(std::string_view event, nlohmann::json fields) {
    if (!events_enabled()) return;
    nlohmann::json line{{"schemaVersion", 1}, {"kind", "mcpp.event"}, {"event", std::string(event)}};
    for (auto it = fields.begin(); it != fields.end(); ++it) line[it.key()] = it.value();
    std::lock_guard lock(g_emit);
    mcpp::ui::terminal::write_protocol_stdout(line.dump() + "\n");
}

}  // namespace mcpp::build::result
