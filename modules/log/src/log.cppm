// mcpp.log — file-based debug logger with verbose terminal output.
//
// Usage:
//   mcpp::log::init(cfg);              // call once at startup
//   mcpp::log::debug("tag", "msg");    // writes to ~/.mcpp/log/mcpp.log
//   mcpp::log::verbose("tag", "msg");  // file + stderr (when --verbose)
//
// Configuration (priority order):
//   1. MCPP_LOG_LEVEL env var: "debug" | "info" | "warn" | "error" | "off"
//   2. config.toml [log].level
//   3. Default: "off"
//
// Log file: <logDir>/mcpp.log (rotated at max_file_size, keeps max_files)

module;
#include <ctime>
#include <cstdio>

export module mcpp.log;

import std;

export namespace mcpp::log {

enum class Level { off, error, warn, info, debug };

struct Config {
    Level                   level        = Level::off;
    std::size_t             maxFileSize  = 10 * 1024 * 1024;  // 10MB
    int                     maxFiles     = 3;
    std::filesystem::path   logDir;
};

void init(const Config& cfg);

void debug(std::string_view tag, std::string_view message);
void info (std::string_view tag, std::string_view message);
void warn (std::string_view tag, std::string_view message);
void error(std::string_view tag, std::string_view message);

// verbose: writes to file (level >= info) AND stderr (when --verbose).
void set_verbose(bool v);
bool is_verbose();
void verbose(std::string_view tag, std::string_view message);

// Scoped verbose timer for diagnosing slow steps (e.g. first-run init / bootstrap
// hangs). Logs "<label>: start" on construction and "<label>: done (Δ=<ms>ms)" on
// destruction, via verbose() — always to the timestamped log file
// (~/.mcpp/log/mcpp.log), and to stderr under --verbose. Cheap: two log lines.
struct ScopedTimer {
    std::string tag;
    std::string label;
    std::chrono::steady_clock::time_point t0;
    ScopedTimer(std::string_view tag_, std::string_view label_);
    ~ScopedTimer();
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    // Emit an intermediate checkpoint: "<label>: <note> (+<ms>ms)".
    void mark(std::string_view note) const;
};

// Check if a level is enabled (avoid constructing expensive messages).
bool is_enabled(Level l);

} // namespace mcpp::log

// ─── Implementation ─────────────────────────────────────────────────

namespace mcpp::log {

namespace {

Level                   g_level    = Level::off;
bool                    g_verbose  = false;
std::filesystem::path   g_logFile;
std::size_t             g_maxFileSize = 10 * 1024 * 1024;
int                     g_maxFiles    = 3;
std::mutex              g_mutex;

Level parse_level(const char* s) {
    if (!s || !*s) return Level::off;
    std::string v(s);
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "debug") return Level::debug;
    if (v == "info")  return Level::info;
    if (v == "warn")  return Level::warn;
    if (v == "error") return Level::error;
    return Level::off;
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

const char* level_str(Level l) {
    switch (l) {
        case Level::debug: return "DEBUG";
        case Level::info:  return "INFO ";
        case Level::warn:  return "WARN ";
        case Level::error: return "ERROR";
        default:           return "     ";
    }
}

void rotate() {
    if (g_logFile.empty() || g_maxFiles <= 0) return;
    std::error_code ec;
    auto size = std::filesystem::file_size(g_logFile, ec);
    if (ec || size < g_maxFileSize) return;

    // mcpp.log.2 → delete, mcpp.log.1 → mcpp.log.2, mcpp.log → mcpp.log.1
    for (int i = g_maxFiles - 1; i >= 1; --i) {
        auto src = g_logFile;
        src += "." + std::to_string(i);
        auto dst = g_logFile;
        dst += "." + std::to_string(i + 1);
        if (i == g_maxFiles - 1) {
            std::filesystem::remove(src, ec);
        } else {
            std::filesystem::rename(src, dst, ec);
        }
    }
    auto first = g_logFile;
    first += ".1";
    std::filesystem::rename(g_logFile, first, ec);
}

void write_log(Level level, std::string_view tag, std::string_view message) {
    if (level > g_level || g_level == Level::off) return;
    if (g_logFile.empty()) return;

    std::lock_guard lock(g_mutex);
    rotate();
    std::ofstream ofs(g_logFile, std::ios::app);
    if (!ofs) return;
    ofs << timestamp() << " [" << level_str(level) << "] "
        << tag << ": " << message << '\n';
}

void write_stderr(std::string_view tag, std::string_view message) {
    // Dim gray for verbose output so it doesn't compete with ui::status.
    // Carry the wall-clock timestamp so a long first-run hang is attributable
    // to a specific step from the live --verbose stream (the log file already
    // has it). Grep-friendly: "[VERBOSE <ts>]".
    std::fprintf(stderr, "\033[2m[VERBOSE %s] %.*s: %.*s\033[0m\n",
        timestamp().c_str(),
        static_cast<int>(tag.size()), tag.data(),
        static_cast<int>(message.size()), message.data());
}

} // namespace

void init(const Config& cfg) {
    // Priority: env var > --verbose > config > default
    if (auto* e = std::getenv("MCPP_LOG_LEVEL"); e && *e) {
        g_level = parse_level(e);
    } else if (g_verbose && cfg.level == Level::off) {
        g_level = Level::info;  // --verbose auto-enables info
    } else {
        g_level = cfg.level;
    }

    g_maxFileSize = cfg.maxFileSize;
    g_maxFiles    = cfg.maxFiles;

    if (g_level == Level::off && !g_verbose) return;

    std::error_code ec;
    std::filesystem::create_directories(cfg.logDir, ec);
    g_logFile = cfg.logDir / "mcpp.log";

    // Only write session marker on first init (avoid duplicate from re-init)
    static bool session_started = false;
    if (!session_started) {
        session_started = true;
        write_log(Level::info, "log",
            std::format("=== session start (level={} verbose={}) ===",
                level_str(g_level), g_verbose));
    }
}

void set_verbose(bool v) {
    g_verbose = v;
    // If verbose enabled but file logging off, auto-enable info level
    if (v && g_level == Level::off) {
        g_level = Level::info;
        if (!g_logFile.empty()) return;
        // Deferred: logDir not set yet, init() will handle it
    }
}

bool is_verbose() { return g_verbose; }

bool is_enabled(Level l) {
    return l <= g_level && g_level != Level::off;
}

void debug(std::string_view tag, std::string_view message) {
    write_log(Level::debug, tag, message);
}

void info(std::string_view tag, std::string_view message) {
    write_log(Level::info, tag, message);
}

void warn(std::string_view tag, std::string_view message) {
    write_log(Level::warn, tag, message);
}

void error(std::string_view tag, std::string_view message) {
    write_log(Level::error, tag, message);
}

void verbose(std::string_view tag, std::string_view message) {
    write_log(Level::info, tag, message);
    if (g_verbose) {
        write_stderr(tag, message);
    }
}

ScopedTimer::ScopedTimer(std::string_view tag_, std::string_view label_)
    : tag(tag_), label(label_), t0(std::chrono::steady_clock::now()) {
    verbose(tag, std::format("{}: start", label));
}

ScopedTimer::~ScopedTimer() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    verbose(tag, std::format("{}: done (Δ={}ms)", label, ms));
}

void ScopedTimer::mark(std::string_view note) const {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    verbose(tag, std::format("{}: {} (+{}ms)", label, note, ms));
}

} // namespace mcpp::log
