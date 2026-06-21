// mcpp.xlings — unified abstraction layer for all xlings (external package
// manager) interactions. Consolidates NDJSON event parsing, subprocess
// command building, path helpers, and bootstrap progress types that were
// previously scattered across config.cppm, package_fetcher.cppm, cli.cppm,
// flags.cppm, ninja_backend.cppm, and stdmod.cppm.
//
// This module is a LEAF dependency: it only imports `std` and
// `mcpp.pm.compat`. It must NOT import mcpp.config or any other mcpp module.

module;
#include <cstdio>      // stderr
#include <cstdlib>

export module mcpp.xlings;

import std;
import mcpp.pm.compat;
import mcpp.platform;

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
    inline constexpr std::string_view kXlingsVersion   = "0.4.51";
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

    // Find a sibling package across all index prefixes.
    // e.g. find_sibling_package(gcc_bin, "linux-headers") searches for
    // xim-x-linux-headers, scode-x-linux-headers, etc.
    // Metadata-only dirs (.xim-installed/.xpkg.lua husks left by delegating
    // index packages) never qualify; when requiredRelPath is given, only a
    // version dir containing it qualifies (the payload may live under a
    // different prefix than the husk — issue #120).
    std::optional<std::filesystem::path>
    find_sibling_package(const std::filesystem::path& compilerBin,
                         std::string_view packageName,
                         std::string_view requiredRelPath = {});

    // xpkgs root of the ACTIVE mcpp home ($MCPP_HOME or ~/.mcpp). Payload
    // discovery consults this in addition to compiler siblings: an
    // inherited/symlinked compiler resolves into its owner home, while the
    // active home may own (or have just installed) the sysroot payloads.
    std::optional<std::filesystem::path> active_home_xpkgs();

    // Like find_sibling_tool, but anchored at the active home's xpkgs.
    // Searches across index prefixes (xim-x-, scode-x-, …) with the same
    // husk/requiredRelPath rules as find_sibling_package.
    std::optional<std::filesystem::path>
    find_home_tool(std::string_view tool,
                   std::string_view requiredRelPath = {});

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
// When not `quiet` and stderr is a TTY, an elapsed-time spinner is shown
// during the (otherwise silent) direct install so first-run doesn't look
// frozen.
int install_with_progress(const Env& env, std::string_view target,
                          const BootstrapProgressCallback& cb,
                          bool quiet = false);

// Run direct `xlings install <target> -y`.
// Used as a fallback when the NDJSON interface install path fails.
int install_direct(const Env& env, std::string_view target, bool quiet = false);

// ─── Sandbox lifecycle ──────────────────────────────────────────────

// Write .xlings.json seed file.
//
// TODO(mirror-default): the default `"CN"` is the historical setting for
// the project's initial Chinese-mainland user base, but it bites overseas
// users (and CI on GitHub-hosted runners) — the first network roundtrip
// goes through a CN mirror that is slow/unreachable for them. The
// `mcpp self config --mirror X` flow now passes the user's choice as an
// override through to here, so they can pick the right mirror BEFORE the
// first download. Longer term, consider:
//   (a) flip the default to "GLOBAL" and have CN users opt in via
//       `mcpp self config --mirror CN` (smaller blast radius once docs
//       cover the switch); or
//   (b) auto-detect on first init (env hint like LANG, a quick HEAD probe
//       to github.com vs. ghproxy with a tight timeout, and pin the
//       winning value into .xlings.json).
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

// Check whether the default mcpplibs index data exists and is fresh
// (within ttlSeconds).
// Returns true if index is present and fresh, false otherwise.
bool is_index_fresh(const Env& env, std::int64_t ttlSeconds);

// Check whether xlings' official xim index data exists and is fresh.
// This is separate from mcpp's default mcpplibs index because xlings
// toolchains live in xim-pkgindex, while modular libraries live in mcpplibs.
bool is_official_index_fresh(const Env& env, std::int64_t ttlSeconds);

// Check whether a specific package file exists in xlings' official xim index
// and the index is fresh. This catches restored CI caches that have an index
// directory and marker but predate a package added later.
bool is_official_package_index_fresh(const Env& env,
                                     std::string_view packageName,
                                     std::int64_t ttlSeconds);

// Run `xlings update` to refresh all index repos. Streams output to stdout.
// Returns the xlings exit code.
int update_index(const Env& env, bool quiet = false);

// Ensure the local index is present and fresh. Runs `xlings update` if
// the index is missing or older than ttlSeconds. Idempotent and quiet
// when no update is needed.
void ensure_index_fresh(const Env& env, std::int64_t ttlSeconds, bool quiet = false);

// Ensure xlings' official xim index is present and fresh.
void ensure_official_index_fresh(const Env& env, std::int64_t ttlSeconds, bool quiet = false);

// Ensure a specific package file exists in xlings' official xim index.
void ensure_official_package_index_fresh(const Env& env,
                                         std::string_view packageName,
                                         std::int64_t ttlSeconds,
                                         bool quiet = false);

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

std::filesystem::path default_index_dir(const Env& env) {
    return paths::index_data(env) / "mcpplibs";
}

std::filesystem::path official_index_dir(const Env& env) {
    return paths::index_data(env) / "xim-pkgindex";
}

std::filesystem::path index_pkgs_dir(const std::filesystem::path& indexDir) {
    return indexDir / "pkgs";
}

std::filesystem::path index_refresh_marker(const std::filesystem::path& indexDir) {
    return indexDir / ".mcpp-index-updated";
}

std::filesystem::path official_package_file(const Env& env, std::string_view packageName) {
    if (packageName.empty()) return {};
    std::string name(packageName);
    return official_index_dir(env) / "pkgs" / std::string(1, name[0]) / (name + ".lua");
}

std::string json_escaped_path_probe(std::filesystem::path path) {
    auto value = path.string();
    std::string escaped;
    escaped.reserve(value.size() * 2);
    for (char c : value) {
        if (c == '\\') escaped += "\\\\";
        else escaped.push_back(c);
    }
    return escaped;
}

bool official_index_cache_matches_package_file(const Env& env,
                                               std::string_view packageName) {
    auto cache = official_index_dir(env) / ".xlings-index-cache.json";
    if (!std::filesystem::exists(cache)) return true;

    auto pkg = official_package_file(env, packageName);
    if (pkg.empty()) return false;

    std::ifstream is(cache);
    if (!is) return false;
    std::string body((std::istreambuf_iterator<char>(is)), {});
    auto rawPath = pkg.string();
    return body.find(rawPath) != std::string::npos
        || body.find(json_escaped_path_probe(pkg)) != std::string::npos;
}

void mark_index_refreshed(const std::filesystem::path& indexDir) {
    if (!std::filesystem::exists(index_pkgs_dir(indexDir))) return;
    std::error_code ec;
    std::filesystem::create_directories(indexDir, ec);
    auto marker = index_refresh_marker(indexDir);
    {
        std::ofstream os(marker, std::ios::trunc);
        if (!os) return;
        os << "ok\n";
    }
    std::filesystem::last_write_time(
        marker, std::filesystem::file_time_type::clock::now(), ec);
}

void mark_known_indexes_refreshed(const Env& env) {
    if (!env.projectDir.empty()) return;
    mark_index_refreshed(default_index_dir(env));
    mark_index_refreshed(official_index_dir(env));
}

bool is_index_dir_fresh(const std::filesystem::path& indexDir, std::int64_t ttlSeconds) {
    std::error_code ec;
    if (!std::filesystem::exists(index_pkgs_dir(indexDir))) return false;

    auto marker = index_refresh_marker(indexDir);
    if (!std::filesystem::exists(marker)) return false;

    auto newest = std::filesystem::last_write_time(marker, ec);
    if (ec) return false;

    auto now = std::filesystem::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - newest);
    return age.count() < ttlSeconds;
}

void write_file(const std::filesystem::path& p, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream os(p);
    os << content;
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (ch < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(ch));
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    return out;
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
    auto r = mcpp::platform::process::capture(cmd);
    if (r.exit_code != 0 && r.output.empty())
        return std::unexpected("command failed: " + cmd);
    return r.output;
}

// ─── Shell quoting ──────────────────────────────────────────────────

std::string shq(std::string_view s) {
    return mcpp::platform::shell::quote(s);
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

std::optional<std::filesystem::path> active_home_xpkgs() {
    std::filesystem::path home;
    if (const char* h = std::getenv("MCPP_HOME"); h && *h) {
        home = h;
    } else if (const char* u = std::getenv("HOME"); u && *u) {
        home = std::filesystem::path(u) / ".mcpp";
    } else {
        return std::nullopt;
    }
    auto xpkgs = home / "registry" / "data" / "xpkgs";
    std::error_code ec;
    if (!std::filesystem::exists(xpkgs, ec)) return std::nullopt;
    return xpkgs;
}

namespace {

// A version dir qualifies as a payload only if it has real content —
// dot-prefixed entries (.xim-installed, .xpkg.lua) are install metadata,
// and a dir holding nothing else is the husk a delegating index package
// leaves behind (the payload lives under another prefix; issue #120).
// When requiredRelPath is given, the dir must also contain that path.
bool payload_dir_qualifies(const std::filesystem::path& versionDir,
                           std::string_view requiredRelPath) {
    std::error_code ec;
    bool hasContent = false;
    for (auto& f : std::filesystem::directory_iterator(versionDir, ec)) {
        if (!f.path().filename().string().starts_with(".")) {
            hasContent = true;
            break;
        }
    }
    if (!hasContent) return false;
    if (!requiredRelPath.empty()
        && !std::filesystem::exists(versionDir / requiredRelPath, ec))
        return false;
    return true;
}

// Scan an xpkgs root across index prefixes (xim-x-, scode-x-, compat-x-, …)
// for the first qualifying version dir of `packageName`.
std::optional<std::filesystem::path>
find_package_in_xpkgs(const std::filesystem::path& xpkgs,
                      std::string_view packageName,
                      std::string_view requiredRelPath) {
    std::error_code ec;
    std::string suffix = std::format("-x-{}", packageName);
    for (auto& entry : std::filesystem::directory_iterator(xpkgs, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto name = entry.path().filename().string();
        if (!name.ends_with(suffix)) continue;
        for (auto& v : std::filesystem::directory_iterator(entry.path(), ec)) {
            if (!v.is_directory(ec)) continue;
            if (payload_dir_qualifies(v.path(), requiredRelPath))
                return v.path();
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::filesystem::path>
find_home_tool(std::string_view tool, std::string_view requiredRelPath) {
    auto xpkgs = active_home_xpkgs();
    if (!xpkgs) return std::nullopt;
    return find_package_in_xpkgs(*xpkgs, tool, requiredRelPath);
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

std::optional<std::filesystem::path>
find_sibling_package(const std::filesystem::path& compilerBin,
                     std::string_view packageName,
                     std::string_view requiredRelPath) {
    auto xpkgs = xpkgs_from_compiler(compilerBin);
    if (!xpkgs) return std::nullopt;

    // Search across index prefixes: xim-x-, scode-x-, compat-x-, etc.
    if (auto found = find_package_in_xpkgs(*xpkgs, packageName, requiredRelPath))
        return found;

    // Also check ~/.xlings/data/xpkgs/ (xlings global home) as fallback.
    std::error_code ec;
    const char* home = std::getenv("HOME");
    if (home) {
        auto xlingsXpkgs = std::filesystem::path(home) / ".xlings" / "data" / "xpkgs";
        if (xlingsXpkgs != *xpkgs && std::filesystem::exists(xlingsXpkgs, ec))
            return find_package_in_xpkgs(xlingsXpkgs, packageName, requiredRelPath);
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
    if constexpr (mcpp::platform::is_windows) {
        mcpp::platform::env::set("XLINGS_HOME", env.home.string());
        mcpp::platform::env::set("XLINGS_PROJECT_DIR",
                  env.projectDir.empty() ? "" : env.projectDir.string());
        mcpp::platform::windows::prepend_path(xvmBin);
        return env.binary.string();
    } else {
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
}

std::string build_interface_command(const Env& env,
                                    std::string_view capability,
                                    std::string_view argsJson) {
    return std::format("{} interface {} --args {} {}",
        build_command_prefix(env), capability, shq(argsJson),
        mcpp::platform::null_redirect);
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

    CallResult result;
    int rc = mcpp::platform::process::run_streaming(cmd,
        [&](std::string_view line) {
            if (line.empty()) return;

            auto ev = parse_event_line(line);
            if (!ev) return;

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
        });
    if (rc != 0 && result.exitCode == 0) result.exitCode = rc;
    return result;
}

// ─── install_with_progress ──────────────────────────────────────────

int install_with_progress(const Env& env, std::string_view target,
                          const BootstrapProgressCallback& cb,
                          bool quiet)
{
    auto argsJson = std::format(
        R"({{"targets":["{}"],"yes":true}})", target);

    // All platforms: try direct `xlings install ... -y` first.
    // The direct command is more reliable for large packages (e.g. LLVM
    // ~800MB) because:
    //   - it doesn't pipe through NDJSON interface (simpler subprocess chain)
    //   - xlings manages its own stdin/stdout/stderr
    //   - extraction subprocess coordination works normally
    // The NDJSON interface path is kept as a fallback for progress reporting.
    {
        auto directCmd = build_command_prefix(env) +
            std::format(" install {} -y {}", target, mcpp::platform::shell::silent_redirect);
        // Windows: explicitly seal stdin (<NUL) so xlings and any grandchildren
        // (curl, git, 7z, etc.) cannot block on a terminal read. The earlier
        // comment claimed POSIX must keep stdin open for "subprocess
        // coordination" — that's never been observed in practice on Linux/macOS,
        // and on Windows it caused users to press Enter repeatedly during first-
        // run toolchain install. POSIX keeps the original behavior to stay
        // conservative.
        if constexpr (mcpp::platform::is_windows) {
            directCmd += " <NUL";
        }

        // The direct install redirects all output to the null device, so it
        // produces zero feedback — on a slow/network-bound first run this
        // looks frozen. Run the blocking std::system() on a worker thread and
        // paint an in-place elapsed-time spinner on stderr while it runs.
        // Only when interactive (not quiet, stderr/stdout is a TTY).
        const bool showSpinner = !quiet && mcpp::platform::terminal::is_tty();

        std::atomic<bool> done{false};
        int directRaw = 0;
        std::thread worker([&] {
            directRaw = std::system(directCmd.c_str());
            done.store(true, std::memory_order_release);
        });

        if (showSpinner) {
            constexpr std::string_view frames = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏";
            // Each braille frame is 3 bytes in UTF-8.
            constexpr std::size_t kFrameBytes = 3;
            const std::size_t nFrames = frames.size() / kFrameBytes;
            const auto start = std::chrono::steady_clock::now();
            std::size_t i = 0;
            while (!done.load(std::memory_order_acquire)) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start).count();
                std::print(stderr, "\r  {} installing {}  ({}s)\x1b[K",
                    frames.substr((i % nFrames) * kFrameBytes, kFrameBytes),
                    target, elapsed);
                std::fflush(stderr);
                ++i;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            // Clear the spinner line.
            std::print(stderr, "\r\x1b[K");
            std::fflush(stderr);
        }

        worker.join();
        int directRc = mcpp::platform::process::extract_exit_code(directRaw);
        if (directRc == 0) return 0;
    }

    // Fallback: NDJSON interface path (provides progress callbacks).
    auto cmd = [&]() -> std::string {
        if constexpr (mcpp::platform::is_windows) {
            return std::format("{} interface install_packages --args {} {}",
                build_command_prefix(env),
                shq(argsJson),
                mcpp::platform::null_redirect);
        } else {
            return std::format(
                "cd {} && env -u XLINGS_PROJECT_DIR XLINGS_HOME={} {} interface install_packages --args {} {}",
                shq(env.home.string()),
                shq(env.home.string()),
                shq(env.binary.string()),
                shq(argsJson),
                mcpp::platform::null_redirect);
        }
    }();

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

    int closeRc = mcpp::platform::process::run_streaming(cmd, handle_line);
    return (resultExitCode != -1) ? resultExitCode : closeRc;
}

int install_direct(const Env& env, std::string_view target, bool quiet) {
    auto cmd = build_command_prefix(env)
        + std::format(" install {} -y", shq(target));
    if (quiet) {
        cmd += " ";
        cmd += std::string(mcpp::platform::shell::silent_redirect);
    }
    if constexpr (mcpp::platform::is_windows) {
        cmd += " <NUL";
    } else {
        cmd += " </dev/null";
    }
    return mcpp::platform::process::extract_exit_code(std::system(cmd.c_str()));
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
                            json_escape(repos[i].first),
                            json_escape(repos[i].second),
                            i + 1 == repos.size() ? "" : ",");
    }
    json += "  ],\n";
    json += "  \"lang\": \"en\",\n";
    json += std::format("  \"mirror\": \"{}\"\n", json_escape(mirror));
    json += "}\n";
    write_file(path, json);
}

int config_show(const Env& env) {
    auto cmd = std::format("{} config", build_command_prefix(env));
    return mcpp::platform::process::run_silent(cmd);
}

int config_set_mirror(const Env& env, std::string_view mirror, bool quiet) {
    if (mirror.empty()) return 0;
    auto cmd = std::format(
        "{} config --mirror {} {}",
        build_command_prefix(env),
        shq(mirror),
        quiet ? mcpp::platform::shell::silent_redirect : "");
    return mcpp::platform::process::run_silent(cmd);
}

void ensure_init(const Env& env, bool quiet) {
    auto marker = paths::sandbox_init_marker(env);
    if (std::filesystem::exists(marker)) return;

    // Ensure the home directory exists before cd'ing into it.
    std::error_code ec;
    std::filesystem::create_directories(env.home, ec);

    if (!quiet)
        print_status("Initialize", "mcpp sandbox layout (one-time)");
    std::string cmd;
    if constexpr (mcpp::platform::is_windows) {
        mcpp::platform::env::set("XLINGS_HOME", env.home.string());
        mcpp::platform::env::set("XLINGS_PROJECT_DIR", "");
        cmd = env.binary.string() + " self init "
            + std::string(mcpp::platform::shell::silent_redirect);
    } else {
        cmd = std::format(
            "cd {} && env -u XLINGS_PROJECT_DIR XLINGS_HOME={} {} self init {}",
            shq(env.home.string()),
            shq(env.home.string()),
            shq(env.binary.string()),
            mcpp::platform::shell::silent_redirect);
    }
    int rc = mcpp::platform::process::run_silent(cmd);
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: `xlings self init` failed for sandbox at '{}'",
            env.home.string());
    }

    // The first real `xlings install` (the next bootstrap step) triggers
    // xlings to fetch its package index — a one-time, network-bound step
    // with no output of its own. Announce it so the user knows the silent
    // wait that follows is expected. This runs once: ensure_init is gated
    // by the sandbox-init marker above.
    if (!quiet)
        print_status("Fetching", "package index (one-time)");
}

void ensure_patchelf(const Env& env, bool quiet,
                     const BootstrapProgressCallback& cb)
{
    auto toolDir = paths::xim_tool(env, "patchelf", pinned::kPatchelfVersion);
    auto binary  = toolDir / "bin" / "patchelf";
    if (std::filesystem::exists(binary)) return;

    // Clean up incomplete installation residue (e.g. from Ctrl+C interrupt).
    if (std::filesystem::exists(toolDir)) {
        if (!quiet)
            print_status("Repairing", "patchelf (incomplete installation, cleaning up)");
        std::error_code ec;
        std::filesystem::remove_all(toolDir, ec);
    }

    if (!quiet)
        print_status("Bootstrap", "patchelf into mcpp sandbox (one-time)");
    int rc = install_with_progress(env,
        std::format("xim:patchelf@{}", pinned::kPatchelfVersion), cb, quiet);
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
    auto ninja_name = std::string("ninja") + std::string(mcpp::platform::exe_suffix);
    if (std::filesystem::exists(root)) {
        std::error_code ec;
        for (auto& v : std::filesystem::directory_iterator(root, ec)) {
            if (std::filesystem::exists(v.path() / ninja_name)) return;
        }
        // Directory exists but no version has a working binary — residue.
        if (!quiet)
            print_status("Repairing", "ninja (incomplete installation, cleaning up)");
        std::filesystem::remove_all(root, ec);
    }
    if (!quiet)
        print_status("Bootstrap", "ninja into mcpp sandbox (one-time)");
    int rc = install_with_progress(env,
        std::format("xim:ninja@{}", pinned::kNinjaVersion), cb, quiet);
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: failed to bootstrap ninja into mcpp sandbox (exit {})",
            rc);
    }
}

// ─── Index freshness ────────────────────────────────────────────────

bool is_index_fresh(const Env& env, std::int64_t ttlSeconds) {
    return is_index_dir_fresh(default_index_dir(env), ttlSeconds);
}

bool is_official_index_fresh(const Env& env, std::int64_t ttlSeconds) {
    return is_index_dir_fresh(official_index_dir(env), ttlSeconds);
}

bool is_official_package_index_fresh(const Env& env,
                                     std::string_view packageName,
                                     std::int64_t ttlSeconds) {
    if (!is_official_index_fresh(env, ttlSeconds)) return false;
    auto pkg = official_package_file(env, packageName);
    return !pkg.empty()
        && std::filesystem::exists(pkg)
        && official_index_cache_matches_package_file(env, packageName);
}

int update_index(const Env& env, bool quiet) {
    std::string cmd = build_command_prefix(env) + " update 2>&1";
    int rc = mcpp::platform::process::run_streaming(cmd,
        [quiet](std::string_view line) {
            if (!quiet) std::println("{}", line);
        });
    if (rc == 0) mark_known_indexes_refreshed(env);
    return rc;
}

void ensure_index_fresh(const Env& env, std::int64_t ttlSeconds, bool quiet) {
    if (is_index_fresh(env, ttlSeconds)) return;
    if (!quiet)
        print_status("Updating", "package index (auto-refresh)");
    update_index(env, /*quiet=*/true);
}

void ensure_official_index_fresh(const Env& env, std::int64_t ttlSeconds, bool quiet) {
    if (is_official_index_fresh(env, ttlSeconds)) return;
    if (!quiet)
        print_status("Updating", "package index (auto-refresh)");
    update_index(env, /*quiet=*/true);
}

void ensure_official_package_index_fresh(const Env& env,
                                         std::string_view packageName,
                                         std::int64_t ttlSeconds,
                                         bool quiet) {
    if (is_official_package_index_fresh(env, packageName, ttlSeconds)) return;
    if (!quiet)
        print_status("Updating", "package index (auto-refresh)");
    update_index(env, /*quiet=*/true);
}

} // namespace mcpp::xlings
