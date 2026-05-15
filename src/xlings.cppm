// mcpp.xlings — unified abstraction layer for all xlings (external package
// manager) interactions. Consolidates NDJSON event parsing, subprocess
// command building, path helpers, and bootstrap progress types that were
// previously scattered across config.cppm, package_fetcher.cppm, cli.cppm,
// flags.cppm, ninja_backend.cppm, and stdmod.cppm.
//
// This module is a LEAF dependency: it only imports `std` and
// `mcpp.pm.compat`. It must NOT import mcpp.config or any other mcpp module.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.xlings;

import std;
import mcpp.pm.compat;

export namespace mcpp::xlings {

// ─── Env: resolved xlings binary + home directory ───────────────────

struct Env {
    std::filesystem::path binary;      // xlings binary path
    std::filesystem::path home;        // XLINGS_HOME directory
    std::filesystem::path projectDir;  // XLINGS_PROJECT_DIR (empty = global mode)
};

// ─── Pinned version constants ───────────────────────────────────────

namespace pinned {
    inline constexpr std::string_view kPatchelfVersion = "0.18.0";
    inline constexpr std::string_view kNinjaVersion    = "1.12.1";
    inline constexpr std::string_view kXlingsVersion   = "0.4.31";
}

// ─── Path helpers (pure functions, no subprocess) ───────────────────

namespace paths {

    // xpkgs base: env.home / "data" / "xpkgs"
    std::filesystem::path xpkgs_base(const Env& env);

    // sandbox bin: env.home / "subos" / "default" / "bin"
    std::filesystem::path sandbox_bin(const Env& env);

    // sandbox sysroot: env.home / "subos" / "default"
    std::filesystem::path sysroot(const Env& env);

    // xim tool root: xpkgs_base / "xim-x-<tool>"
    std::filesystem::path xim_tool_root(const Env& env, std::string_view tool);

    // xim tool versioned: xpkgs_base / "xim-x-<tool>" / "<version>"
    std::filesystem::path xim_tool(const Env& env, std::string_view tool,
                                   std::string_view version);

    // From compiler binary, climb parent dirs to find "xpkgs" directory.
    // Replaces 3 duplicate implementations in flags.cppm, ninja_backend.cppm,
    // stdmod.cppm.
    std::optional<std::filesystem::path>
    xpkgs_from_compiler(const std::filesystem::path& compilerBin);

    // Find a sibling xim tool relative to a compiler binary.
    // e.g. find_sibling_tool(gcc_bin, "binutils") returns highest version
    // dir of xim-x-binutils.
    std::optional<std::filesystem::path>
    find_sibling_tool(const std::filesystem::path& compilerBin,
                      std::string_view tool);

    // Find a binary inside a sibling tool (e.g. binutils/bin/ar,
    // ninja/ninja).
    std::optional<std::filesystem::path>
    find_sibling_binary(const std::filesystem::path& compilerBin,
                        std::string_view tool,
                        std::string_view binaryRelPath);

    // index data root: env.home / "data"
    std::filesystem::path index_data(const Env& env);

    // sandbox init marker: env.home / "subos" / "default" / ".xlings.json"
    std::filesystem::path sandbox_init_marker(const Env& env);

} // namespace paths

// ─── Shell quoting ──────────────────────────────────────────────────

// Shell-escape (single-quote) a string for the command line.
std::string shq(std::string_view s);

// ─── Shell command builders ─────────────────────────────────────────

// Build the standard xlings command prefix with proper env vars.
// cd '<home>' && env -u XLINGS_PROJECT_DIR PATH=<sandbox_bin>:"$PATH"
//     XLINGS_HOME='<home>' '<binary>'
std::string build_command_prefix(const Env& env);

// Build full xlings interface command.
// <prefix> interface <capability> --args '<argsJson>' 2>/dev/null
std::string build_interface_command(const Env& env,
                                    std::string_view capability,
                                    std::string_view argsJson);

// ─── NDJSON event types ─────────────────────────────────────────────

struct ProgressEvent {
    std::string  phase;      // "download", "extract", "configure", ...
    int          percent;    // 0..100
    std::string  message;
};

struct LogEvent {
    std::string  level;      // "debug" | "info" | "warn" | "error"
    std::string  message;
};

struct DataEvent {
    std::string  dataKind;   // "install_plan", "styled_list", ...
    std::string  payloadJson;// raw JSON (let caller parse)
};

struct ErrorEvent {
    std::string  code;
    std::string  message;
    std::string  hint;
    bool         recoverable = false;
};

struct ResultEvent {
    int          exitCode = 0;
    std::string  dataJson;   // additional payload, may be empty
};

using Event = std::variant<ProgressEvent, LogEvent, DataEvent,
                           ErrorEvent, ResultEvent>;

// Parse one NDJSON line into an Event.
std::optional<Event> parse_event_line(std::string_view line);

// ─── JSON extraction helpers (for NDJSON parsing) ───────────────────

std::string extract_string(std::string_view text, std::string_view key);
std::optional<long long> extract_int(std::string_view text, std::string_view key);
std::optional<bool> extract_bool(std::string_view text, std::string_view key);
std::string extract_object(std::string_view text, std::string_view key);

// ─── Subprocess call ────────────────────────────────────────────────

struct CallResult {
    int                          exitCode = 0;
    std::vector<DataEvent>       dataEvents;
    std::optional<ErrorEvent>    error;
    std::string                  resultJson;
};

struct EventHandler {
    virtual ~EventHandler() = default;
    virtual void on_progress(const ProgressEvent&) {}
    virtual void on_log     (const LogEvent&)     {}
    virtual void on_data    (const DataEvent&)    {}
    virtual void on_error   (const ErrorEvent&)   {}
    virtual void on_result  (const ResultEvent&)  {}
};

std::expected<CallResult, std::string>
call(const Env& env, std::string_view capability,
     std::string_view argsJson, EventHandler* handler = nullptr);

// ─── Bootstrap progress types ───────────────────────────────────────

struct BootstrapFile {
    std::string  name;             // xim package id, e.g. "xim:patchelf@0.18.0"
    double       downloadedBytes = 0;
    double       totalBytes      = 0;
    bool         started         = false;
    bool         finished        = false;
};

struct BootstrapProgress {
    std::vector<BootstrapFile>  files;
    double                      elapsedSec = 0;
};

using BootstrapProgressCallback = std::function<void(const BootstrapProgress&)>;

// Run xlings install with progress callback (used by bootstrap functions).
int install_with_progress(const Env& env, std::string_view target,
                          const BootstrapProgressCallback& cb);

// ─── Sandbox lifecycle ──────────────────────────────────────────────

// Write .xlings.json seed file.
void seed_xlings_json(const Env& env,
                      std::span<const std::pair<std::string,std::string>> repos,
                      std::string_view mirror = "CN");

// Persist the xlings mirror selection in .xlings.json via xlings itself.
int config_show(const Env& env);
int config_set_mirror(const Env& env, std::string_view mirror, bool quiet = false);

// Run xlings self init.
void ensure_init(const Env& env, bool quiet);

// Ensure patchelf is installed.
void ensure_patchelf(const Env& env, bool quiet,
                     const BootstrapProgressCallback& cb);

// Ensure ninja is installed.
void ensure_ninja(const Env& env, bool quiet,
                  const BootstrapProgressCallback& cb);

// ─── Index freshness ────────────────────────────────────────────────

// Check whether local index data exists and is fresh (within ttlSeconds).
// Returns true if index is present and fresh, false otherwise.
bool is_index_fresh(const Env& env, std::int64_t ttlSeconds);

// Run `xlings update` to refresh all index repos. Streams output to stdout.
// Returns the xlings exit code.
int update_index(const Env& env, bool quiet = false);

// Ensure the local index is present and fresh. Runs `xlings update` if
// the index is missing or older than ttlSeconds. Idempotent and quiet
// when no update is needed.
void ensure_index_fresh(const Env& env, std::int64_t ttlSeconds, bool quiet = false);

// ─── run_capture utility ────────────────────────────────────────────

std::expected<std::string, std::string> run_capture(const std::string& cmd);

} // namespace mcpp::xlings

// ═══════════════════════════════════════════════════════════════════════
// Implementation
// ═══════════════════════════════════════════════════════════════════════

namespace mcpp::xlings {

namespace {

// Right-pad a verb to 12 columns for bootstrap status lines.
void print_status(std::string_view verb, std::string_view msg) {
    constexpr std::size_t W = 12;
    if (verb.size() >= W) {
        std::println("{} {}", verb, msg);
    } else {
        std::println("{}{} {}", std::string(W - verb.size(), ' '), verb, msg);
    }
}

void write_file(const std::filesystem::path& p, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream os(p);
    os << content;
}

// LineScan: cheap field extraction for bootstrap install progress lines.
// Handles flat JSON; no nested array/object — the keys we extract are
// all leaves.
struct LineScan {
    std::string_view s;
    std::string find_str(std::string_view key) const {
        std::string n = std::format("\"{}\":\"", key);
        auto p = s.find(n);
        if (p == std::string_view::npos) return "";
        p += n.size();
        std::string out;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) {
                out.push_back(s[p+1]); p += 2; continue;
            }
            out.push_back(s[p++]);
        }
        return out;
    }
    double find_num(std::string_view key) const {
        std::string n = std::format("\"{}\":", key);
        auto p = s.find(n);
        if (p == std::string_view::npos) return 0;
        p += n.size();
        auto e = p;
        while (e < s.size()
            && (std::isdigit(static_cast<unsigned char>(s[e]))
                || s[e] == '.' || s[e] == '-' || s[e] == '+'
                || s[e] == 'e' || s[e] == 'E')) ++e;
        try { return std::stod(std::string(s.substr(p, e - p))); }
        catch (...) { return 0; }
    }
    bool find_bool(std::string_view key) const {
        std::string n = std::format("\"{}\":", key);
        auto p = s.find(n);
        if (p == std::string_view::npos) return false;
        p += n.size();
        return s.size() - p >= 4 && s.substr(p, 4) == "true";
    }
};

} // anonymous namespace

// ─── run_capture ────────────────────────────────────────────────────

std::expected<std::string, std::string> run_capture(const std::string& cmd) {
    std::array<char, 4096> buf{};
    std::string out;
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return std::unexpected("popen failed: " + cmd);
    while (std::fgets(buf.data(), buf.size(), fp) != nullptr) out += buf.data();
    int rc = ::pclose(fp);
    if (rc != 0 && out.empty()) return std::unexpected("command failed: " + cmd);
    return out;
}

// ─── Shell quoting ──────────────────────────────────────────────────

std::string shq(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// ─── Path helpers ───────────────────────────────────────────────────

namespace paths {

std::filesystem::path xpkgs_base(const Env& env) {
    return env.home / "data" / "xpkgs";
}

std::filesystem::path sandbox_bin(const Env& env) {
    return env.home / "subos" / "default" / "bin";
}

std::filesystem::path sysroot(const Env& env) {
    return env.home / "subos" / "default";
}

std::filesystem::path xim_tool_root(const Env& env, std::string_view tool) {
    return xpkgs_base(env) / std::format("xim-x-{}", tool);
}

std::filesystem::path xim_tool(const Env& env, std::string_view tool,
                               std::string_view version) {
    return xpkgs_base(env) / std::format("xim-x-{}", tool) / std::string(version);
}

std::optional<std::filesystem::path>
xpkgs_from_compiler(const std::filesystem::path& compilerBin) {
    for (auto p = compilerBin.parent_path();
         p.has_parent_path() && p != p.root_path();
         p = p.parent_path()) {
        if (p.filename() == "xpkgs") return p;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path>
find_sibling_tool(const std::filesystem::path& compilerBin,
                  std::string_view tool) {
    auto xpkgs = xpkgs_from_compiler(compilerBin);
    if (!xpkgs) return std::nullopt;

    auto root = *xpkgs / std::format("xim-x-{}", tool);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;

    // Return the first (highest) version dir that exists.
    for (auto& v : std::filesystem::directory_iterator(root, ec)) {
        if (v.is_directory(ec)) return v.path();
    }
    return std::nullopt;
}

std::optional<std::filesystem::path>
find_sibling_binary(const std::filesystem::path& compilerBin,
                    std::string_view tool,
                    std::string_view binaryRelPath) {
    auto xpkgs = xpkgs_from_compiler(compilerBin);
    if (!xpkgs) return std::nullopt;

    auto root = *xpkgs / std::format("xim-x-{}", tool);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;

    for (auto& v : std::filesystem::directory_iterator(root, ec)) {
        auto candidate = v.path() / std::string(binaryRelPath);
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return std::nullopt;
}

std::filesystem::path index_data(const Env& env) {
    return env.home / "data";
}

std::filesystem::path sandbox_init_marker(const Env& env) {
    return env.home / "subos" / "default" / ".xlings.json";
}

} // namespace paths

// ─── Shell command builders ─────────────────────────────────────────

std::string build_command_prefix(const Env& env) {
    auto xvmBin = paths::sandbox_bin(env).string();
    if (env.projectDir.empty()) {
        // Global mode: unset XLINGS_PROJECT_DIR (existing behavior).
        return std::format(
            "cd {} && env -u XLINGS_PROJECT_DIR PATH={}:\"$PATH\" XLINGS_HOME={} {}",
            shq(env.home.string()),
            shq(xvmBin),
            shq(env.home.string()),
            shq(env.binary.string()));
    }
    // Project-level mode: set XLINGS_PROJECT_DIR so xlings uses
    // additive project repos alongside global repos.
    return std::format(
        "cd {} && env PATH={}:\"$PATH\" XLINGS_HOME={} XLINGS_PROJECT_DIR={} {}",
        shq(env.home.string()),
        shq(xvmBin),
        shq(env.home.string()),
        shq(env.projectDir.string()),
        shq(env.binary.string()));
}

std::string build_interface_command(const Env& env,
                                    std::string_view capability,
                                    std::string_view argsJson) {
    return std::format("{} interface {} --args {} 2>/dev/null",
        build_command_prefix(env), capability, shq(argsJson));
}

// ─── JSON extraction helpers ────────────────────────────────────────

std::string extract_string(std::string_view text, std::string_view key) {
    auto needle = std::string{"\""} + std::string(key) + "\":\"";
    auto p = text.find(needle);
    if (p == std::string_view::npos) return "";
    p += needle.size();
    std::string out;
    while (p < text.size()) {
        char c = text[p++];
        if (c == '\\' && p < text.size()) {
            char nc = text[p++];
            switch (nc) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '"': out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                default: out.push_back(nc);
            }
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<long long> extract_int(std::string_view text, std::string_view key) {
    auto needle = std::string{"\""} + std::string(key) + "\":";
    auto p = text.find(needle);
    if (p == std::string_view::npos) return std::nullopt;
    p += needle.size();
    while (p < text.size() && text[p] == ' ') ++p;
    bool neg = false;
    if (p < text.size() && text[p] == '-') { neg = true; ++p; }
    long long n = 0;
    bool any = false;
    while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]))) {
        n = n * 10 + (text[p++] - '0');
        any = true;
    }
    if (!any) return std::nullopt;
    return neg ? -n : n;
}

std::optional<bool> extract_bool(std::string_view text, std::string_view key) {
    auto needle = std::string{"\""} + std::string(key) + "\":";
    auto p = text.find(needle);
    if (p == std::string_view::npos) return std::nullopt;
    p += needle.size();
    while (p < text.size() && text[p] == ' ') ++p;
    if (text.substr(p, 4) == "true")  return true;
    if (text.substr(p, 5) == "false") return false;
    return std::nullopt;
}

std::string extract_object(std::string_view text, std::string_view key) {
    auto needle = std::string{"\""} + std::string(key) + "\":";
    auto p = text.find(needle);
    if (p == std::string_view::npos) return "";
    p += needle.size();
    while (p < text.size() && text[p] == ' ') ++p;
    if (p >= text.size() || (text[p] != '{' && text[p] != '[')) return "";
    char open  = text[p];
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    std::size_t start = p;
    bool in_string = false;
    while (p < text.size()) {
        char c = text[p];
        if (in_string) {
            if (c == '\\' && p + 1 < text.size()) { p += 2; continue; }
            if (c == '"') in_string = false;
            ++p; continue;
        }
        if (c == '"') { in_string = true; ++p; continue; }
        if (c == open)  { ++depth; }
        else if (c == close) {
            --depth;
            if (depth == 0) return std::string(text.substr(start, p - start + 1));
        }
        ++p;
    }
    return "";
}

// ─── NDJSON event parser ────────────────────────────────────────────

std::optional<Event> parse_event_line(std::string_view line) {
    auto kind = extract_string(line, "kind");
    if (kind == "progress") {
        ProgressEvent e;
        e.phase   = extract_string(line, "phase");
        e.percent = static_cast<int>(extract_int(line, "percent").value_or(0));
        e.message = extract_string(line, "message");
        return e;
    }
    if (kind == "log") {
        LogEvent e;
        e.level   = extract_string(line, "level");
        e.message = extract_string(line, "message");
        return e;
    }
    if (kind == "data") {
        DataEvent e;
        e.dataKind   = extract_string(line, "dataKind");
        e.payloadJson= extract_object(line, "payload");
        return e;
    }
    if (kind == "error") {
        ErrorEvent e;
        e.code        = extract_string(line, "code");
        e.message     = extract_string(line, "message");
        e.hint        = extract_string(line, "hint");
        e.recoverable = extract_bool(line, "recoverable").value_or(false);
        return e;
    }
    if (kind == "result") {
        ResultEvent e;
        e.exitCode = static_cast<int>(extract_int(line, "exitCode").value_or(0));
        return e;
    }
    // heartbeat and unknown kinds
    return std::nullopt;
}

// ─── Subprocess call ────────────────────────────────────────────────

std::expected<CallResult, std::string>
call(const Env& env, std::string_view capability,
     std::string_view argsJson, EventHandler* handler)
{
    auto cmd = build_interface_command(env, capability, argsJson);

    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return std::unexpected(
        std::format("failed to spawn xlings: {}", cmd));

    CallResult result;
    std::array<char, 16384> buf{};
    std::string acc;
    while (std::fgets(buf.data(), buf.size(), fp) != nullptr) {
        acc += buf.data();
        std::size_t pos;
        while ((pos = acc.find('\n')) != std::string::npos) {
            auto line = acc.substr(0, pos);
            acc.erase(0, pos + 1);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            auto ev = parse_event_line(line);
            if (!ev) continue;

            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ProgressEvent>) {
                    if (handler) handler->on_progress(e);
                } else if constexpr (std::is_same_v<T, LogEvent>) {
                    if (handler) handler->on_log(e);
                } else if constexpr (std::is_same_v<T, DataEvent>) {
                    result.dataEvents.push_back(e);
                    if (handler) handler->on_data(e);
                } else if constexpr (std::is_same_v<T, ErrorEvent>) {
                    result.error = e;
                    if (handler) handler->on_error(e);
                } else if constexpr (std::is_same_v<T, ResultEvent>) {
                    result.exitCode = e.exitCode;
                    result.resultJson = e.dataJson;
                    if (handler) handler->on_result(e);
                }
            }, *ev);
        }
    }
    int rc = ::pclose(fp);
    if (rc != 0 && result.exitCode == 0) result.exitCode = rc;
    return result;
}

// ─── install_with_progress ──────────────────────────────────────────

int install_with_progress(const Env& env, std::string_view target,
                          const BootstrapProgressCallback& cb)
{
    auto argsJson = std::format(
        R"({{"targets":["{}"],"yes":true}})", target);

    auto cmd = std::format(
        "cd {} && env -u XLINGS_PROJECT_DIR XLINGS_HOME={} {} interface install_packages --args {} 2>/dev/null",
        shq(env.home.string()),
        shq(env.home.string()),
        shq(env.binary.string()),
        shq(argsJson));

    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return -1;

    std::array<char, 16384> buf{};
    std::string acc;
    int resultExitCode = -1;

    auto handle_line = [&](std::string_view line) {
        LineScan ls{line};
        auto kind = ls.find_str("kind");
        if (kind == "result") {
            resultExitCode = static_cast<int>(ls.find_num("exitCode"));
            return;
        }
        if (kind != "data") return;
        if (ls.find_str("dataKind") != "download_progress") return;
        if (!cb) return;

        auto p = line.find("\"files\":[");
        if (p == std::string_view::npos) return;
        p += 9;

        BootstrapProgress prog;
        prog.elapsedSec = ls.find_num("elapsedSec");

        while (p < line.size()) {
            while (p < line.size() && (line[p] == ' ' || line[p] == '\n'
                                       || line[p] == ',')) ++p;
            if (p >= line.size() || line[p] == ']') break;
            if (line[p] != '{') break;
            int depth = 0;
            auto start = p;
            bool in_string = false;
            for (; p < line.size(); ++p) {
                char c = line[p];
                if (in_string) {
                    if (c == '\\' && p + 1 < line.size()) { ++p; continue; }
                    if (c == '"') in_string = false;
                    continue;
                }
                if (c == '"')      in_string = true;
                else if (c == '{') ++depth;
                else if (c == '}') { if (--depth == 0) { ++p; break; } }
            }
            LineScan fl{line.substr(start, p - start)};
            BootstrapFile f;
            f.name            = fl.find_str("name");
            f.downloadedBytes = fl.find_num("downloadedBytes");
            f.totalBytes      = fl.find_num("totalBytes");
            f.started         = fl.find_bool("started");
            f.finished        = fl.find_bool("finished");
            if (!f.name.empty()) prog.files.push_back(std::move(f));
        }
        if (!prog.files.empty()) cb(prog);
    };

    while (std::fgets(buf.data(), buf.size(), fp)) {
        acc += buf.data();
        std::size_t pos;
        while ((pos = acc.find('\n')) != std::string::npos) {
            handle_line(std::string_view{acc}.substr(0, pos));
            acc.erase(0, pos + 1);
        }
    }
    if (!acc.empty()) handle_line(acc);
    int closeRc = ::pclose(fp);
    return (resultExitCode != -1) ? resultExitCode : closeRc;
}

// ─── Sandbox lifecycle ──────────────────────────────────────────────

void seed_xlings_json(const Env& env,
                      std::span<const std::pair<std::string,std::string>> repos,
                      std::string_view mirror)
{
    auto path = env.home / ".xlings.json";
    std::string json = "{\n";
    json += "  \"index_repos\": [\n";
    for (std::size_t i = 0; i < repos.size(); ++i) {
        json += std::format("    {{ \"name\": \"{}\", \"url\": \"{}\" }}{}\n",
                            repos[i].first, repos[i].second,
                            i + 1 == repos.size() ? "" : ",");
    }
    json += "  ],\n";
    json += "  \"lang\": \"en\",\n";
    json += std::format("  \"mirror\": \"{}\"\n", mirror);
    json += "}\n";
    write_file(path, json);
}

int config_show(const Env& env) {
    auto cmd = std::format("{} config", build_command_prefix(env));
    return std::system(cmd.c_str());
}

int config_set_mirror(const Env& env, std::string_view mirror, bool quiet) {
    if (mirror.empty()) return 0;
    auto cmd = std::format(
        "{} config --mirror {} {}",
        build_command_prefix(env),
        shq(mirror),
        quiet ? ">/dev/null 2>&1" : "");
    return std::system(cmd.c_str());
}

void ensure_init(const Env& env, bool quiet) {
    auto marker = paths::sandbox_init_marker(env);
    if (std::filesystem::exists(marker)) return;

    if (!quiet)
        print_status("Initialize", "mcpp sandbox layout (one-time)");
    auto cmd = std::format(
        "cd {} && env -u XLINGS_PROJECT_DIR XLINGS_HOME={} {} self init >/dev/null 2>&1",
        shq(env.home.string()),
        shq(env.home.string()),
        shq(env.binary.string()));
    int rc = std::system(cmd.c_str());
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: `xlings self init` failed for sandbox at '{}'",
            env.home.string());
    }
}

void ensure_patchelf(const Env& env, bool quiet,
                     const BootstrapProgressCallback& cb)
{
    auto marker = paths::xim_tool(env, "patchelf", pinned::kPatchelfVersion)
                / "bin" / "patchelf";
    if (std::filesystem::exists(marker)) return;

    if (!quiet)
        print_status("Bootstrap", "patchelf into mcpp sandbox (one-time)");
    int rc = install_with_progress(env,
        std::format("xim:patchelf@{}", pinned::kPatchelfVersion), cb);
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: failed to bootstrap patchelf into mcpp sandbox; "
            "subsequent xim installs may skip ELF rewriting");
    }
}

void ensure_ninja(const Env& env, bool quiet,
                  const BootstrapProgressCallback& cb)
{
    auto root = paths::xim_tool_root(env, "ninja");
    if (std::filesystem::exists(root)) {
        std::error_code ec;
        for (auto& v : std::filesystem::directory_iterator(root, ec)) {
            if (std::filesystem::exists(v.path() / "ninja")) return;
        }
    }
    if (!quiet)
        print_status("Bootstrap", "ninja into mcpp sandbox (one-time)");
    int rc = install_with_progress(env,
        std::format("xim:ninja@{}", pinned::kNinjaVersion), cb);
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: failed to bootstrap ninja into mcpp sandbox (exit {})",
            rc);
    }
}

// ─── Index freshness ────────────────────────────────────────────────

bool is_index_fresh(const Env& env, std::int64_t ttlSeconds) {
    auto data = paths::index_data(env);
    if (!std::filesystem::exists(data)) return false;

    // Look for any directory under data/ that has a pkgs/ subdirectory —
    // that's a cloned index repo.
    std::error_code ec;
    bool hasIndex = false;
    std::filesystem::file_time_type newest{};
    for (auto& entry : std::filesystem::directory_iterator(data, ec)) {
        if (!entry.is_directory()) continue;
        auto pkgsDir = entry.path() / "pkgs";
        if (!std::filesystem::exists(pkgsDir)) continue;
        hasIndex = true;
        auto t = std::filesystem::last_write_time(pkgsDir, ec);
        if (!ec && t > newest) newest = t;
    }
    if (!hasIndex) return false;

    // Check TTL
    auto now = std::filesystem::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - newest);
    return age.count() < ttlSeconds;
}

int update_index(const Env& env, bool quiet) {
    std::string cmd = build_command_prefix(env) + " update 2>&1";
    std::array<char, 4096> buf{};
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return -1;
    while (std::fgets(buf.data(), buf.size(), fp)) {
        if (!quiet) std::fputs(buf.data(), stdout);
    }
    return ::pclose(fp);
}

void ensure_index_fresh(const Env& env, std::int64_t ttlSeconds, bool quiet) {
    if (is_index_fresh(env, ttlSeconds)) return;
    if (!quiet)
        print_status("Updating", "package index (auto-refresh)");
    update_index(env, quiet);
}

} // namespace mcpp::xlings
