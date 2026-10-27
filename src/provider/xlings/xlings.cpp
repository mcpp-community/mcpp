// mcpp.provider.xlings — the implementation: how mcpp starts xlings, reads
// its events and keeps its home.
module;
#include <cstdio>      // stderr
#include <cstdlib>

module mcpp.provider.xlings;

import std;
import mcpp.provider;
import mcpp.pm.compat;
import mcpp.pm.index_contract;
import mcpplibs.os.platform;
import mcpp.invocation;
import mcpp.effects;
import mcpp.build.command_line;
import mcpp.run;
import mcpp.runtime.launch_env;
import mcpp.home.files;
import mcpp.log;
import mcpp.ui;                 // closing notices of the refresh guard
import mcpp.home;
import mcpp.xpkg_version;
import mcpp.libs.json;
import mcpplibs.os.fs;

namespace mcpp::provider::xlings {
// How mcpp starts xlings: `args` after the binary, the environment
// `invocation_env` decides with the sandbox's `bin` in front of PATH, and the
// home as the working directory (#726). The environment is the started
// program's alone: mcpp's own is the same after the invocation as before it,
// so a build that installed a payload starts ninja with the environment a
// build that installed nothing does (the sandbox's `bin` holds the shims
// `xim:llvm` registers on Windows -- `cl`, `link`, `lib`, `rc` -- which
// vcpkg's compiler detection met in front of MSVC's tools when the PATH
// prefix outlived an invocation).
struct Invocation {
    std::vector<std::string> argv;
    mcpp::run::Options       options;
};
Invocation invocation(const Env& env, std::vector<std::string> args);

}  // namespace mcpp::provider::xlings

// ═══════════════════════════════════════════════════════════════════════
// Implementation
// ═══════════════════════════════════════════════════════════════════════

namespace mcpp::provider::xlings {

namespace {

// Right-pad a verb to 12 columns for bootstrap status lines, on standard
// error: the stream mcpp.ui narrates on.
void print_status(std::string_view verb, std::string_view msg) {
    constexpr std::size_t W = 12;
    if (verb.size() >= W) {
        std::println(stderr, "{} {}", verb, msg);
    } else {
        std::println(stderr, "{}{} {}", std::string(W - verb.size(), ' '), verb, msg);
    }
}

std::filesystem::path default_index_dir(const Env& env) {
    return paths::index_dir_of(env, "mcpplibs");
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

std::filesystem::path index_version_file(const std::filesystem::path& indexDir) {
    return indexDir / ".xlings-index-version";
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
    // Project-scoped envs used to return early here, which left the global
    // indexes unmarked whenever a project with custom `[indices]` triggered the
    // sync — so `mcpp index status` reported "unknown" forever on exactly the
    // machines that refresh most. The marker is advisory (it debounces and it
    // dates the status line; it is NOT what decides whether a refresh is
    // needed), so marking it from either env is both safe and more accurate.
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
    // A marker stamped in the FUTURE yields a negative age, which the plain
    // `age < ttl` test read as "fresh" — and stayed fresh until the wall clock
    // caught up, potentially for years. Future timestamps are routine: clock
    // skew in containers/VMs, a tar that preserved mtimes, a restored CI cache.
    // An unusable timestamp means "unknown", and unknown must mean stale.
    if (age.count() < 0) return false;
    return age.count() < ttlSeconds;
}

// Seconds since the index's refresh marker was last touched, or -1 if the
// marker is missing/unreadable. Read-only — no network, no side effects.
std::int64_t index_age_seconds(const std::filesystem::path& indexDir) {
    std::error_code ec;
    auto marker = index_refresh_marker(indexDir);
    auto newest = std::filesystem::last_write_time(marker, ec);
    if (ec) return -1;
    auto now = std::filesystem::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - newest).count();
    return age < 0 ? -1 : age;   // future stamp → "unknown", same rule as above
}

// Defined inside the anonymous namespace so `index_status_for` (also here) can
// use it; the exported `index_revision` below forwards to it.
std::optional<std::string> read_index_revision(const std::filesystem::path& indexDir) {
    std::ifstream is(index_version_file(indexDir), std::ios::binary);
    if (!is) return std::nullopt;
    std::string body((std::istreambuf_iterator<char>(is)), {});
    // The observed files carry no trailing newline, but trim anyway: this value
    // is compared for equality and printed, and a stray \r from a Windows-side
    // writer would otherwise turn "same rev" into "changed rev" every run.
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!body.empty() && isSpace(static_cast<unsigned char>(body.back())))
        body.pop_back();
    std::size_t b = 0;
    while (b < body.size() && isSpace(static_cast<unsigned char>(body[b]))) ++b;
    body.erase(0, b);
    if (body.empty()) return std::nullopt;
    return body;
}

IndexStatus index_status_for(const std::filesystem::path& indexDir,
                             std::int64_t ttlSeconds) {
    std::error_code ec;
    bool present = std::filesystem::exists(index_pkgs_dir(indexDir), ec) && !ec;
    return IndexStatus{
        .dir        = indexDir,
        .present    = present,
        .fresh      = is_index_dir_fresh(indexDir, ttlSeconds),
        .ageSeconds = index_age_seconds(indexDir),
        .rev        = read_index_revision(indexDir),
    };
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

// ─── Index identity ─────────────────────────────────────────────────

std::optional<std::string> index_revision(const std::filesystem::path& indexDir) {
    return read_index_revision(indexDir);
}

// ─── The handshake ──────────────────────────────────────────────────

mcpp::provider::ProviderInfo describe(const std::filesystem::path& binary,
                                    std::chrono::milliseconds budget) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + budget;
    // Older clients can adopt or journal a home before answering a query.
    // The handshake describes the binary, so ask it outside the user's home.
    static std::mutex mutex;
    static std::map<std::tuple<std::filesystem::path, std::uintmax_t,
        std::filesystem::file_time_type>, mcpp::provider::ProviderInfo> cache;
    std::lock_guard lock(mutex);
    mcpp::provider::ProviderInfo info;
    std::error_code ec;
    const auto executable = std::filesystem::absolute(binary, ec);
    if (ec) return info;
    const auto size = std::filesystem::file_size(executable, ec);
    if (ec) return info;
    const auto modified = std::filesystem::last_write_time(executable, ec);
    if (ec) return info;
    const auto key = std::tuple{executable, size, modified};
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    // AND ACROSS PROCESSES (design 2026-10-26 §8): the answer describes the
    // binary, so it is kept under the home keyed as the in-process cache is,
    // by the file's path, size and modification time. Every query (`emit
    // build-database`, `toolchain list`) started a process for it.
    const auto parse = [&](const nlohmann::json& j) {
        const auto strings = [&](const char* field) {
            std::vector<std::string> values;
            if (j.contains(field) && j[field].is_array())
                for (const auto& value : j[field]) if (value.is_string()) values.push_back(value.get<std::string>());
            return values;
        };
        info.version = j.value("version", std::string{});
        info.protocol = j.value("protocol_version", std::string{});
        info.capabilities = strings("capabilities");
        info.features = strings("features");
        info.homeLayouts.clear();
        if (j.contains("home_layouts") && j["home_layouts"].is_array())
            for (const auto& value : j["home_layouts"]) if (value.is_number_integer()) info.homeLayouts.push_back(value.get<int>());
        info.answered = !info.version.empty();
    };
    const auto stamp = std::format("{}\t{}\t{}", executable.generic_string(),
        static_cast<std::uint64_t>(size), static_cast<std::int64_t>(modified.time_since_epoch().count()));
    const auto persisted = mcpp::home::root() / "cache" / "provider-handshake"
                         / (std::format("{:016x}", std::hash<std::string>{}(stamp)) + ".json");
    if (std::ifstream in(persisted, std::ios::binary); in) {
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_object() && j.value("stamp", std::string{}) == stamp && j.contains("answer")) {
            parse(j["answer"]);
            if (info.answered) {
                cache.emplace(key, info);
                return info;
            }
            info = {};
        }
    }
    const auto temporary = std::filesystem::temp_directory_path(ec);
    if (ec) return info;
    const auto home = temporary / std::format("mcpp-provider-handshake-{}-{}",
        steady_clock::now().time_since_epoch().count(), std::random_device{}());
    if (!std::filesystem::create_directory(home, ec) || ec) return info;
    struct Cleanup {
        std::filesystem::path home;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(home, error); }
    } cleanup{home};
    try {
        const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now());
        if (remaining <= milliseconds::zero()) return info;
        auto inv = invocation(Env{executable, home, {}}, { "interface", "--version" });
        inv.argv.front() = mcpp::home::spelling(executable);
        inv.options.cwd = mcpp::home::spelling(home);
        inv.options.streams = mcpp::run::Streams::output;
        inv.options.total = remaining;
        inv.options.report_spawn_failure = false;
        const auto output = mcpp::run::execute(inv.argv, inv.options);
        const bool timedOut = output.timed_out;
        const auto j = !timedOut && output.exit_code == 0
            ? nlohmann::json::parse(output.output, nullptr, false) : nlohmann::json{};
        if (j.is_object()) {
            parse(j);
            // Written only by a run that writes the global cache, for the
            // reason the probe cache gives.
            if (info.answered && mcpp::effects::writes_global_cache()
                && std::filesystem::is_directory(mcpp::home::root(), ec)) {
                std::filesystem::create_directories(persisted.parent_path(), ec);
                auto tmp = persisted;
                tmp += std::format(".{:x}", std::random_device{}());
                { std::ofstream(tmp, std::ios::binary) << nlohmann::json{{"stamp", stamp}, {"answer", j}}.dump(); }
                std::filesystem::rename(tmp, persisted, ec);
                if (ec) std::filesystem::remove(tmp, ec);
            }
        }
    } catch (const std::exception&) { info = {}; }
    cache.emplace(key, info);
    return info;
}

std::string unmet(const mcpp::provider::ProviderInfo& info) {
    if (!info.answered) return "answers no handshake (an xlings before 2026.10.16.1)";
    if (mcpp::xpkg_version::compare_keys(info.version, pinned::kXlingsVersion) < 0)
        return std::format("is {}, older than {}", info.version, pinned::kXlingsVersion);
    for (auto feature : {"index_dir", "index_store", "upstreams", "payload_status", "sync_index",
                         "payload_currency", "binding_state", "network_diagnostics",
                         "download_socket_budgets", "read_only_queries"})
        if (std::ranges::find(info.features, feature) == info.features.end())
            return std::format("does not offer `{}`", feature);
    if (std::ranges::find(info.capabilities, "install_packages") == info.capabilities.end())
        return "does not offer `install_packages`";
    return {};
}

// ─── Shell quoting ──────────────────────────────────────────────────

std::string shq(std::string_view s) {
    return mcpp::build::command_line::quote(s);
}

// The same, for an argument that may carry a shell metacharacter -- which the
// JSON payloads below do, and which a version constraint spelled `>=` does.
// See `quote_windows_through_cmd` for what `shq` alone leaves unprotected.
std::string shq_meta(std::string_view s) {
    return mcpp::build::command_line::quote_through_shell(s);
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

// Parse `[<ns>:]<name>[@<version>]` exactly as a manifest writes it.
XpkgRef parse_xpkg_ref(std::string_view spec) { return mcpp::provider::parse_ref(spec); }

// Where that package's payload is, or nullopt if it is not installed.
//
// THE ANSWER IS XLINGS' ANSWER. The address was resolved by xlings, so the
// payload it selected is found with xlings' grammar (mcpp.xpkg_version), not
// with the Cargo grammar mcpp reads its own dependencies with. They disagree
// where it matters (#712): `libglvnd@1.7` installed `1.7.0.1`, and the Cargo
// reading of `1.7` could not see a four-segment directory at all, so
// `mcpp::xpkg_dir` answered "" for a payload that was on disk.
//
// The request selects among the installed directories exactly as xlings would
// among index keys (`select_installed`): a literal name first, then a bare
// version of three or more segments as written-prefix equality (1.8.12 matches
// 1.8.12.x and never 1.9.0), one or two segments as a prefix range, operators
// as ranges; with no version, the highest installed one.
std::optional<std::filesystem::path>
xpkg_payload(const Env& env, const XpkgRef& ref) {
    if (auto recorded = recorded_payload(env, ref)) return recorded;
    for (const auto& store : stores(env))
        if (auto found = xpkg_payload_at(store, ref)) return found;
    return std::nullopt;
}

std::optional<std::filesystem::path>
xpkg_payload_at(const std::filesystem::path& xpkgsBase, const XpkgRef& ref) {
    if (ref.name.empty()) return std::nullopt;
    const auto root = xpkgsBase / std::format("{}-x-{}", ref.ns, ref.name);
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return std::nullopt;
    std::vector<std::string> installed;
    for (auto const& e : std::filesystem::directory_iterator(root, ec))
        if (e.is_directory(ec)) installed.push_back(e.path().filename().string());
    auto pick = mcpp::xpkg_version::select_installed(installed, ref.version);
    if (!pick) return std::nullopt;
    return root / *pick;
}


// ─── Resolution records ────────────────────────────────────────────

namespace {

// One file per (xpkgs base, address). The address is canonicalised through
// parse_xpkg_ref, so `libglvnd@1.7` and `xim:libglvnd@1.7` share a record, and
// the base is part of the key, so two homes never answer for each other.
std::filesystem::path record_path(const Env& env, const XpkgRef& ref) {
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a
    const auto key = std::format("{}\n{}:{}@{}",
        xpkgs_base(env).generic_string(), ref.ns, ref.name, ref.version);
    for (unsigned char ch : key) { h ^= ch; h *= 1099511628211ull; }
    return mcpp::home::root() / "provisioned" / "resolved"
         / std::format("{:016x}.json", h);
}

} // namespace

std::vector<ResolvedTarget> parse_install_targets(std::string_view payloadJson) {
    std::vector<ResolvedTarget> out;
    auto j = nlohmann::json::parse(payloadJson, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.contains("targets") || !j["targets"].is_array()) return out;
    auto str = [](const nlohmann::json& o, const char* k) {
        return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string{};
    };
    for (auto const& t : j["targets"]) {
        if (!t.is_object()) continue;
        ResolvedTarget r;
        r.request    = str(t, "request");
        r.ns         = str(t, "namespace");
        r.name       = str(t, "name");
        r.version    = str(t, "version");
        r.status     = str(t, "status");
        r.payloadDir = str(t, "payload_dir");
        if (t.contains("revision") && t["revision"].is_number_integer())
            r.revision = t["revision"].get<int>();
        if (!r.request.empty()) out.push_back(std::move(r));
    }
    return out;
}

void record_resolutions(const Env& env, std::span<const ResolvedTarget> targets) {
    for (auto const& t : targets) {
        if (t.status == "failed" || t.payloadDir.empty()) continue;
        std::error_code ec;
        if (!std::filesystem::is_directory(t.payloadDir, ec)) continue;
        const auto path = record_path(env, parse_xpkg_ref(t.request));
        std::filesystem::create_directories(path.parent_path(), ec);
        nlohmann::json j;
        j["request"]     = t.request;
        j["namespace"]   = t.ns;
        j["name"]        = t.name;
        j["version"]     = t.version;
        j["revision"]    = t.revision;
        j["payload_dir"] = t.payloadDir.generic_string();
        auto tmp = path;
        tmp += std::format(".{}.tmp",
            std::chrono::steady_clock::now().time_since_epoch().count());
        {
            std::ofstream o{tmp, std::ios::binary | std::ios::trunc};
            if (!o) continue;
            o << j.dump();
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) std::filesystem::remove(tmp, ec);
    }
}

std::optional<int> installed_revision(const std::filesystem::path& payloadDir) {
    std::ifstream in{payloadDir / ".xpkg-install.json", std::ios::binary};
    if (!in) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(in), {}};
    auto j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    if (!j.contains("revision") || !j["revision"].is_number_integer()) return 0;
    return j["revision"].get<int>();
}

std::optional<std::filesystem::path>
recorded_payload(const Env& env, const XpkgRef& ref) {
    if (ref.name.empty()) return std::nullopt;
    std::ifstream in{record_path(env, ref), std::ios::binary};
    if (!in) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(in), {}};
    auto j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.contains("payload_dir") || !j["payload_dir"].is_string())
        return std::nullopt;
    const std::filesystem::path dir = j["payload_dir"].get<std::string>();
    // A record answers only for a payload that is still there and still this
    // package's, in this env's store. Anything else -- the payload was
    // removed, the home moved -- falls through to the grammar, and the
    // provisioning check treats "no answer" as not installed (#716).
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return std::nullopt;
    for (const auto& store : stores(env)) {
        const auto root = store / std::format("{}-x-{}", ref.ns, ref.name);
        if (dir.parent_path().lexically_normal() == root.lexically_normal()) return dir;
    }
    return std::nullopt;
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
subos_dir_of(const std::filesystem::path& compilerBin) {
    auto xpkgs = xpkgs_from_compiler(compilerBin);
    if (!xpkgs) return std::nullopt;
    // <home>/data/xpkgs → <home>/subos/default. Spelled from the xpkgs dir
    // rather than from Env so a toolchain inherited from ANOTHER home resolves
    // to that home's subos, which is the one whose payloads the binary was
    // actually linked against.
    auto home = xpkgs->parent_path().parent_path();
    if (home.empty()) return std::nullopt;
    return home / "subos" / "default";
}

std::optional<std::filesystem::path>
find_sibling_tool(const std::filesystem::path& compilerBin,
                  std::string_view tool) {
    auto xpkgs = xpkgs_from_compiler(compilerBin);
    if (!xpkgs) return std::nullopt;

    auto root = *xpkgs / std::format("xim-x-{}", tool);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;

    // The first version dir readdir yields. ORDER IS UNDEFINED -- this does
    // NOT return the highest, and the comment that said so was wrong for as
    // long as it stood. With two versions installed the answer varies by
    // filesystem, so a caller that needs a SPECIFIC version must name it;
    // see probe.cppm, where the version comes from the resolved runtime
    // binding rather than from this scan.
    //
    // A test asserting the old "highest" claim was attempted and removed: it
    // passed or failed by directory order, which makes both outcomes
    // uninformative.
    for (auto& v : std::filesystem::directory_iterator(root, ec)) {
        if (v.is_directory(ec)) return v.path();
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> active_home_xpkgs() {
    // `mcpp::home::root()`, NOT A FOURTH DERIVATION OF IT.
    //
    // This function used to resolve the home itself — `$MCPP_HOME`, else
    // `$HOME/.mcpp` — which is two of the three answers `mcpp.home` gives. The
    // one it left out is SELF-CONTAINED MODE: a release tarball or
    // `xlings install mcpp` puts the binary at `<root>/bin/mcpp` and the
    // unpacked tree IS the home. On such an install this function answered
    // `$HOME/.mcpp`, so payload discovery reached a DIFFERENT home than
    // everything else in the process — and what payload discovery produces is
    // `-isystem` rows on every compile command.
    //
    // `mcpp.home`'s own header opens with this: "Every path under the mcpp
    // home must be derived from here. Before #311 this logic existed in three
    // places … the copies drifted." This was the fourth copy, and it drifted
    // the same way.
    auto xpkgs = mcpp::home::root() / "registry" / "data" / "xpkgs";
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

    // THE `~/.xlings` FALLBACK IS GONE, AND ITS REMOVAL IS THE POINT.
    //
    // It used to read: "Also check ~/.xlings/data/xpkgs/ (xlings global home)
    // as fallback." That was written when one machine had one home. It means
    // that a build whose `MCPP_HOME` names one tree can take a payload out of
    // ANOTHER, and what this function's callers do with the result is put it on
    // every compile command:
    //
    //     probe.cppm:448   linux-headers  →  -isystem <other home>/…/include
    //
    // ⇒ a hermetic build reaching outside its own sandbox for headers, with no
    // diagnostic, and with the resulting objects looking exactly like objects
    // built from the payload that was actually pinned. Reported as the second
    // half of mcpp#514, where a project's dependency units carried `-isystem`
    // rows naming a home the build was not using.
    //
    // THE DIRECTION IS THE SAFE ONE. Not finding a payload is reported —
    // `probe.cppm` already has the verbose branch for it, and a glibc build
    // that then fails at `<linux/limits.h>` says so at the first compile.
    // Finding the WRONG one says nothing at all, and this codebase has paid
    // for that shape before (`e2e-inherit-toolchain-corrupts-real-payloads`,
    // `dev-overlay-poisons-a-released-version`).
    return std::nullopt;
}

std::filesystem::path index_data(const Env& env) {
    return env.home / "data";
}

namespace {
std::vector<DeclaredIndex> declared_in(const std::filesystem::path& file, const std::filesystem::path& base,
                                       const std::filesystem::path& dataRoot) {
    std::vector<DeclaredIndex> out;
    std::ifstream in(file);
    if (!in) return out;
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object() || !j.contains("index_repos") || !j["index_repos"].is_array()) return out;
    const auto absolute = [&](const std::string& p) {
        const std::filesystem::path path = p;
        return path.is_absolute() ? path.lexically_normal() : (base / path).lexically_normal();
    };
    for (const auto& e : j["index_repos"]) {
        if (!e.is_object() || !e.contains("name") || !e["name"].is_string()) continue;
        DeclaredIndex d;
        d.name = e["name"].get<std::string>();
        if (e.contains("dir") && e["dir"].is_string() && !e["dir"].get<std::string>().empty()) {
            d.dir = absolute(e["dir"].get<std::string>());
            d.owned = true;
        } else {
            d.dir = dataRoot / d.name;
        }
        if (e.contains("store") && e["store"].is_string() && !e["store"].get<std::string>().empty())
            d.store = absolute(e["store"].get<std::string>());
        out.push_back(std::move(d));
    }
    return out;
}
} // namespace

std::vector<DeclaredIndex> declared_indices(const Env& env) {
    std::vector<DeclaredIndex> out;
    if (!env.projectDir.empty())
        out = declared_in(env.projectDir / ".xlings.json", env.projectDir, env.projectDir / "data");
    for (auto& d : declared_in(env.home / ".xlings.json", env.home, index_data(env)))
        if (std::ranges::none_of(out, [&](const DeclaredIndex& o) { return o.name == d.name; }))
            out.push_back(std::move(d));
    return out;
}

std::filesystem::path index_dir_of(const Env& env, std::string_view name) {
    for (auto& d : declared_indices(env))
        if (d.name == name) return d.dir;
    return index_data(env) / std::string(name);
}

std::vector<std::string> indices_of(const std::filesystem::path& home) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(home / ".xlings.json", ec)) return {};
    std::vector<std::string> out{"xim"};
    for (auto& d : declared_in(home / ".xlings.json", home, home / "data"))
        if (std::ranges::find(out, d.name) == out.end()) out.push_back(d.name);
    return out;
}

std::vector<std::filesystem::path> index_roots_of(const std::filesystem::path& home) {
    const auto data = home / "data";
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    const auto declared = declared_in(home / ".xlings.json", home, data);
    // An owned index is read where its owner keeps it; a copy left at the
    // provider's own place (written by a provider that did not know the
    // owner's place) is not a second index.
    const auto owned_name = [&](const std::filesystem::path& p) {
        return std::ranges::any_of(declared, [&](const DeclaredIndex& d) { return d.owned && d.name == p.filename(); });
    };
    for (auto& e : std::filesystem::directory_iterator(data, ec))
        if (e.is_directory(ec) && !owned_name(e.path())) out.push_back(e.path());
    for (auto& d : declared)
        if (d.owned && std::filesystem::is_directory(d.dir, ec) && std::ranges::find(out, d.dir) == out.end())
            out.push_back(d.dir);
    std::ranges::sort(out);
    return out;
}

std::vector<std::filesystem::path> stores_of(const std::filesystem::path& home) {
    std::vector<std::filesystem::path> out;
    for (auto& d : declared_in(home / ".xlings.json", home, home / "data"))
        if (!d.store.empty() && std::ranges::find(out, d.store) == out.end()) out.push_back(d.store);
    if (auto own = home / "data" / "xpkgs"; std::ranges::find(out, own) == out.end()) out.push_back(own);
    return out;
}

std::vector<std::filesystem::path> index_roots(const Env& env) {
    return index_roots_of(env.projectDir.empty() ? env.home : env.projectDir);
}

void own_indices(const Env& env, std::span<const mcpp::provider::SeedRepo> owned) {
    const bool project = !env.projectDir.empty();
    const auto base = project ? env.projectDir : env.home;
    const auto data = project ? env.projectDir / "data" : index_data(env);
    const auto file = base / ".xlings.json";
    std::ifstream in(file);
    if (!in) return;
    auto j = nlohmann::json::parse(in, nullptr, false);
    in.close();
    if (!j.is_object() || !j.contains("index_repos") || !j["index_repos"].is_array()) return;
    bool changed = false;
    for (const auto& o : owned) {
        if (o.dir.empty()) continue;
        for (auto& e : j["index_repos"]) {
            if (!e.is_object() || e.value("name", std::string{}) != o.name) continue;
            // Its data, from where the provider kept it, once: a rename on one
            // filesystem, a copy and a removal across two.
            std::error_code ec;
            const auto old = data / o.name;
            if (std::filesystem::is_directory(old, ec) && !std::filesystem::exists(o.dir, ec)) {
                std::filesystem::create_directories(o.dir.parent_path(), ec);
                std::filesystem::rename(old, o.dir, ec);
                if (ec) {
                    ec.clear();
                    std::filesystem::copy(old, o.dir, std::filesystem::copy_options::recursive
                                                          | std::filesystem::copy_options::copy_symlinks, ec);
                    if (!ec) std::filesystem::remove_all(old, ec);
                    else std::filesystem::remove_all(o.dir, ec);
                }
            }
            if (e.value("dir", std::string{}) != o.dir.generic_string()) {
                e["dir"] = o.dir.generic_string();
                changed = true;
            }
            if (!o.store.empty() && e.value("store", std::string{}) != o.store.generic_string()) {
                e["store"] = o.store.generic_string();
                changed = true;
            }
        }
    }
    if (!changed) return;
    auto tmp = file;
    tmp += std::format(".mcpp-{:x}", std::random_device{}());
    write_file(tmp, j.dump(2) + "\n");
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

std::vector<std::filesystem::path> stores(const Env& env) {
    std::vector<std::filesystem::path> out;
    for (auto& d : declared_indices(env))
        if (!d.store.empty() && std::ranges::find(out, d.store) == out.end()) out.push_back(d.store);
    if (auto home = xpkgs_base(env); std::ranges::find(out, home) == out.end()) out.push_back(home);
    return out;
}

std::filesystem::path sandbox_init_marker(const Env& env) {
    return env.home / "subos" / "default" / ".xlings.json";
}

} // namespace paths

// ─── Shell command builders ─────────────────────────────────────────

std::vector<InvocationVar> invocation_env(const Env& env) {
    auto cap = [](const char* key, int limit) {
        auto value = mcpp::invocation::get(key);
        int existing = 0;
        if (value) {
            const auto [end, ec] = std::from_chars(value->data(), value->data() + value->size(), existing);
            if (ec == std::errc{} && end == value->data() + value->size() && existing > 0)
                limit = std::min(limit, existing);
        }
        return std::to_string(limit);
    };
    return {
        {"XLINGS_HOME", env.home.string(), true},
        {"XLINGS_PROJECT_DIR", env.projectDir.string(), !env.projectDir.empty()},
        {"XLINGS_ACTIVE_SUBOS", "", false},
        {"XLINGS_DOWNLOAD_CONNECT_TIMEOUT", cap("XLINGS_DOWNLOAD_CONNECT_TIMEOUT", 8), true},
        {"XLINGS_DOWNLOAD_IDLE_TIMEOUT", cap("XLINGS_DOWNLOAD_IDLE_TIMEOUT", 30), true},
        {"XLINGS_DOWNLOAD_LOW_SPEED", mcpp::invocation::get("XLINGS_DOWNLOAD_LOW_SPEED").value_or("off"), true},
    };
}

Invocation invocation(const Env& env, std::vector<std::string> args) {
    Invocation inv;
    inv.argv.reserve(args.size() + 1);
    inv.argv.push_back(env.binary.string());
    for (auto& a : args) inv.argv.push_back(std::move(a));
    for (auto const& var : invocation_env(env)) {
        if (var.present) inv.options.env.emplace_back(var.name, var.value);
        else             inv.options.unset.push_back(var.name);
    }
    auto path = paths::sandbox_bin(env).string();
    if (auto inherited = mcpp::invocation::get("PATH"))
        path += std::string(mcpp::runtime::launch_env::path_list_separator()) + *inherited;
    inv.options.env.emplace_back("PATH", std::move(path));
    inv.options.cwd = env.home.string();
    return inv;
}

void select_runtime(const Env& env, std::string_view runtimeId) {
    auto inv = invocation(env, { "config", "--runtime", std::string(runtimeId) });
    inv.options.streams = mcpp::run::Streams::discard;
    (void)mcpp::run::execute(inv.argv, inv.options);
}

std::string windows_command_prefix(const Env& env) {
    return std::format("cd /d {} && {}",
        mcpp::build::command_line::quote_windows(env.home.string()),
        mcpp::build::command_line::quote_windows(env.binary.string()));
}

std::string build_command_prefix(const Env& env) {
    auto xvmBin = paths::sandbox_bin(env).string();
    if constexpr (mcpplibs::os::is_windows) {
        return windows_command_prefix(env);
    } else {
        // `env` takes its `-u` operands before its assignments.
        std::string unset, assign;
        for (auto const& var : invocation_env(env)) {
            if (var.present) assign += std::format(" {}={}", var.name, shq(var.value));
            else             unset  += std::format(" -u {}", var.name);
        }
        return std::format("cd {} && env{} PATH={}:\"$PATH\"{} {}",
            shq(env.home.string()), unset, shq(xvmBin), assign,
            shq(env.binary.string()));
    }
}

std::string build_interface_command(const Env& env,
                                    std::string_view capability,
                                    std::string_view argsJson) {
    return std::format("{} interface {} --args {} {}",
        build_command_prefix(env), capability, shq_meta(argsJson),
        mcpp::build::command_line::null_redirect);
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

namespace {
std::optional<BootstrapProgress> download_progress_of(std::string_view line);
}

std::expected<CallResult, std::string>
call(const Env& env, std::string_view capability,
     std::string_view argsJson, EventHandler* handler)
{
    if (capability == "system_status" || capability == "payload_status") {
        const auto info = describe(env.binary);
        if (std::ranges::find(info.features, "read_only_queries") == info.features.end())
            return std::unexpected("the provider does not offer read_only_queries; status was not queried");
    }
    // STDERR IS KEPT APART, NOT DISCARDED AND NOT MERGED INTO STDOUT (#614).
    // Stdout is parsed line by line as NDJSON, so merging stderr into it could
    // split an event; discarding it lost the one line that names a rejection.
    // It is read only when the call fails.
    auto inv = invocation(env, { "interface", std::string(capability), "--args", std::string(argsJson) });
    const auto cmd = mcpp::build::command_line::display(inv.argv);

    // #238: under MCPP_VERBOSE=1 surface the exact xlings invocation so a
    // failing install_packages can be reproduced/inspected by hand. The
    // NDJSON child's own error/warn lines are surfaced by the caller's
    // EventHandler (e.g. InstallProgressHandler).
    mcpp::log::verbose("xlings",
        std::format("interface {} exec: {}", capability, cmd));

    // An install reaches the network; the query capabilities read local state.
    if (capability == "install_packages" || capability == "update_packages")
        mcpp::effects::note_network_access();

    CallResult result;
    const bool installing = capability == "install_packages";
    const bool syncing = capability == "sync_index" || capability == "update_packages";
    InstallDeadline deadline;
    std::map<std::string, int> sourceAttempts;
    inv.options.streams = mcpp::run::Streams::output_error_kept;
    inv.options.on_line = [&](std::string_view line) {
            if (line.empty()) return;
            if (installing) {
                deadline.observe(line);
                if (!mcpp::ui::is_quiet())
                    if (const auto progress = download_progress_of(line))
                        for (const auto& file : progress->files)
                            if (!file.sourceHost.empty() && sourceAttempts[file.name] != file.sourceAttempt) {
                                sourceAttempts[file.name] = file.sourceAttempt;
                                print_status("Source", std::format("{} {} (attempt {})",
                                    file.name, file.sourceHost, file.sourceAttempt));
                            }
            }

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
                    // The FIRST error is the cause. A package whose
                    // dependency failed reports that after it (xlings
                    // 2026.10.10.2), and older clients followed a failure with
                    // its consequences ("registered none of its programs").
                    if (!result.error) result.error = e;
                    if (handler) handler->on_error(e);
                } else if constexpr (std::is_same_v<T, ResultEvent>) {
                    result.exitCode = e.exitCode;
                    result.resultJson = e.dataJson;
                    if (handler) handler->on_result(e);
                }
            }, *ev);
        };
    inv.options.total = installing ? std::chrono::duration_cast<std::chrono::milliseconds>(kDirectInstallTimeout)
                      : syncing ? std::chrono::duration_cast<std::chrono::milliseconds>(index_refresh_timeout())
                                : std::chrono::milliseconds{0};
    inv.options.idle = std::chrono::duration_cast<std::chrono::milliseconds>(kInterfaceIdleTimeout);
    inv.options.should_stop = [&] { return installing && deadline.expired(); };
    const auto ran = mcpp::run::execute(inv.argv, inv.options);
    const int rc = ran.exit_code;
    const bool timedOut = ran.timed_out;
    if (timedOut) {
        result.exitCode = result.exitCode != 0 ? result.exitCode : 124;
        const auto message = std::format(
            "provider {} exceeded its progress/process deadline and was stopped", capability);
        if (!result.error) result.error = ErrorEvent{
            installing || syncing ? "E_NETWORK" : "E_PROVIDER_TIMEOUT", message,
            installing || syncing ? "check proxy settings or use mcpp self config --mirror auto|CN|GLOBAL"
                                  : "retry the local provider operation", true};
        result.stderrTail.push_back(message);
    }
    if (rc != 0 && result.exitCode == 0) result.exitCode = rc;
    if (result.exitCode != 0) {
        // Error-level lines only, the last 20: enough to name a rejection, and
        // bounded so a verbose child cannot bury the diagnostic it is attached
        // to.
        auto tail = stderr_error_tail(ran.error_output);
        result.stderrTail.insert(result.stderrTail.end(), tail.begin(), tail.end());
    }
    return result;
}

std::vector<std::string> stderr_error_tail(std::string_view text, std::size_t limit) {
    std::deque<std::string> tail;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto nl = text.find('\n', pos);
        std::string line(text.substr(pos, nl == std::string_view::npos
                                              ? std::string_view::npos : nl - pos));
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string lower = line;
        std::ranges::transform(lower, lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("error") == std::string::npos
            && line.find("E_") == std::string::npos
            && !line.starts_with("[xim]"))
            continue;
        tail.push_back(std::move(line));
        if (tail.size() > limit) tail.pop_front();
    }
    return {tail.begin(), tail.end()};
}

namespace {
// The `files[]` of one NDJSON `download_progress` data event, in the order
// xlings reports them; nullopt when the line carries none. Shared by the
// sandbox bootstrap and the index refresh, which render the same event.
std::optional<BootstrapProgress> download_progress_of(std::string_view line) {
    LineScan ls{line};
    auto p = line.find("\"files\":[");
    if (p == std::string_view::npos) return std::nullopt;
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
        f.sourceHost      = fl.find_str("sourceHost");
        f.sourceAttempt   = static_cast<int>(fl.find_num("sourceAttempt"));
        if (!f.name.empty()) prog.files.push_back(std::move(f));
    }
    if (prog.files.empty()) return std::nullopt;
    return prog;
}

// The events of one `xlings interface update_packages` run, rendered with the
// renderer every other acquisition uses (mcpp::ui::ProgressBar and
// DownloadProgress). A `progress` event carries a phase and a percentage;
// one bar is drawn per phase (xlings reports `index_sync`, one event per
// repository, and `index_rebuild`, one event per descriptor file), so a
// refresh prints a few lines off a terminal rather than one per file. A
// `download_progress` data event is an index artifact being fetched. A line
// that is not an event is not rendered: an xlings that predates structured
// index progress printed its own terminal text on this stream, and the bar
// is what replaces it.
class IndexRefreshRenderer {
public:
    void line(std::string_view text) {
        LineScan ls{text};
        const auto kind = ls.find_str("kind");
        if (kind == "result") {
            resultExit_ = static_cast<int>(ls.find_num("exitCode"));
        } else if (kind == "progress") {
            const auto phase = ls.find_str("phase");
            if (phase.empty()) return;
            // A sync step names its repository at the end of its message
            // (`syncing index repo 2/5: mcpplibs`); each repository is its
            // own bar, labelled with that name.
            std::string subject;
            if (phase == "index_sync") {
                const auto message = ls.find_str("message");
                if (auto colon = message.rfind(": "); colon != std::string::npos)
                    subject = message.substr(colon + 2);
            }
            const auto key = subject.empty() ? phase : phase + "/" + subject;
            if (!bar_ || key != label_) {
                if (bar_) bar_->finish();
                shown_ = subject.empty() ? phase_label(phase)
                                         : std::format("package index {}", subject);
                bar_.emplace("Updating", shown_);
                label_ = key;
            }
            const auto pct = std::clamp(ls.find_num("percent"), 0.0, 100.0);
            bar_->update(static_cast<std::size_t>(pct));
        } else if (kind == "data" && ls.find_str("dataKind") == "download_progress") {
            auto prog = download_progress_of(text);
            if (!prog) return;
            if (bar_) { bar_->finish(); bar_.reset(); label_.clear(); }
            std::vector<mcpp::ui::DownloadFile> files;
            for (auto const& f : prog->files)
                files.push_back({f.name,
                                 static_cast<std::size_t>(f.downloadedBytes),
                                 static_cast<std::size_t>(f.totalBytes),
                                 f.started, f.finished});
            download_.update(files, prog->elapsedSec);
        }
    }
    // The exit code the result event carried, or -1 when none arrived.
    int result_exit() const { return resultExit_; }
    // The run failed or was stopped: the open bar says the step did not
    // complete rather than reporting it done.
    void fail() { failed_ = true; }
    ~IndexRefreshRenderer() {
        if (bar_) failed_ ? bar_->finish_failed(shown_) : bar_->finish();
        failed_ ? download_.finish_failed() : download_.finish();
    }

private:
    static std::string phase_label(std::string_view phase) {
        if (phase == "index_sync")    return "package index (sync)";
        if (phase == "index_rebuild") return "package index (rebuild)";
        return std::format("package index ({})", phase);
    }

    std::optional<mcpp::ui::ProgressBar> bar_;
    std::string                          label_;   // the phase (and repository) drawn
    std::string                          shown_;   // the bar's label
    bool                                 failed_ = false;
    mcpp::ui::DownloadProgress           download_;
    int                                  resultExit_ = -1;
};
} // namespace

void InstallDeadline::observe(std::string_view line, std::chrono::steady_clock::time_point now) {
    const auto event = parse_event_line(line);
    if (!event) return; // heartbeats and unknown events never renew a budget
    if (const auto* progress = std::get_if<ProgressEvent>(&*event)) {
        if (progress->phase == "extract" || progress->phase == "hook" || progress->phase == "configure")
            localWork_ = true;
    } else if (const auto* data = std::get_if<DataEvent>(&*event);
               data && data->dataKind == "download_progress") {
        if (const auto p = download_progress_of(line)) {
            downloads_.observe(*p, now);
            downloadSeen_ |= std::ranges::any_of(p->files, [](const auto& f) { return f.started; });
        }
    }
}

mcpp::provider::NetworkReport network_check(const Env& env, std::chrono::milliseconds budget) {
    using namespace std::chrono;
    using json = nlohmann::json;
    if (mcpp::effects::offline_mode()) return {"offline", {}};
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(env.binary, fileError)) return {"unavailable", {}};
    const auto deadline = steady_clock::now() + budget;
    auto run = [&](std::vector<std::string> args) -> std::optional<std::string> {
        const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now());
        if (remaining <= milliseconds::zero()) return std::nullopt;
        auto inv = invocation(env, std::move(args));
        inv.argv.front() = mcpp::home::spelling(env.binary);
        inv.options.total = remaining;
        inv.options.report_spawn_failure = false;
        const auto r = mcpp::run::execute(inv.argv, inv.options);
        if (r.timed_out || r.exit_code != 0) return std::nullopt;
        return r.output;
    };
    // Do not run an unknown capability on an older provider: its generic CLI
    // may journal or initialize an operation before returning "unsupported".
    const auto info = describe(env.binary, duration_cast<milliseconds>(deadline - steady_clock::now()));
    const auto has = [&](std::string_view feature) {
        return std::ranges::find(info.features, feature) != info.features.end();
    };
    const bool supported = has("network_diagnostics") && has("read_only_queries");
    if (!supported) return {steady_clock::now() >= deadline ? "timeout" : "unsupported", {}};
    auto call = [&](std::string_view operation, const json& args, std::string_view kind) -> json {
        try {
            if (operation == "probe_url") mcpp::effects::note_network_access();
            const auto output = run({"interface", std::string(operation), "--args", args.dump()});
            if (!output) return nullptr;
            std::istringstream lines(*output);
            std::string line;
            while (std::getline(lines, line)) {
                auto event = json::parse(line, nullptr, false);
                if (event.is_object() && event.value("kind", "") == "data"
                    && event.value("dataKind", "") == kind && event.contains("payload"))
                    return event["payload"];
            }
            return nullptr;
        } catch (const std::exception&) { return nullptr; }
    };
    const auto discovered = call("network_endpoints", json::object(), "network_endpoints");
    if (!discovered.is_object() || !discovered.contains("endpoints")
        || !discovered["endpoints"].is_array())
        return {steady_clock::now() >= deadline ? "timeout" : "unsupported", {}};
    const auto& endpoints = discovered["endpoints"];
    mcpp::provider::NetworkReport report{"checked", {}};
    report.endpoints.resize(endpoints.size());
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        for (;;) {
            const auto i = next.fetch_add(1);
            if (i >= endpoints.size()) return;
            const auto& e = endpoints[i];
            auto& out = report.endpoints[i];
            try {
                out.resource = e.value("resource", "");
                out.region = e.value("region", "");
                out.status = "timeout";
                if (!e.contains("url") || !e["url"].is_string()) { out.status = "invalid_endpoint"; continue; }
                const auto data = call("probe_url", json{{"url", e["url"]}, {"socket_timeout_ms", 2000}}, "network_probe");
                if (!data.is_object()) continue;
                out.host = data.value("host", "");
                out.status = data.value("status", "unknown");
                out.httpStatus = data.value("http_status", 0);
                out.elapsedSeconds = data.value("elapsed_seconds", 0.0);
                out.reachable = data.value("reachable", false);
                out.proxy = data.value("proxy", false);
            } catch (const std::exception&) { out.status = "invalid_response"; }
        }
    };
    std::vector<std::jthread> workers;
    for (std::size_t i = 0; i < std::min<std::size_t>(4, endpoints.size()); ++i)
        workers.emplace_back(worker);
    for (auto& w : workers) w.join();
    return report;
}

// ─── install_with_progress ──────────────────────────────────────────

int install_with_progress(const Env& env, std::string_view target,
                          const BootstrapProgressCallback& cb,
                          bool quiet)
{
    const auto argsJson = nlohmann::json{{"targets", {std::string(target)}}, {"yes", true}}.dump();

    // One structured install attempt. Repeating a failed direct install through
    // the interface only retries the same operation and hides its first error.
    if (!quiet) print_status("Installing", target);
    mcpp::effects::note_network_access();
    auto inv = invocation(env, { "interface", "install_packages", "--args", argsJson });

    int resultExitCode = -1;
    std::map<std::string, int> sourceAttempts;
    mcpp::ui::DownloadProgress download;
    InstallDeadline deadline;

    auto handle_line = [&](std::string_view line) {
        deadline.observe(line);
        LineScan ls{line};
        auto kind = ls.find_str("kind");
        if (kind == "result") {
            resultExitCode = static_cast<int>(ls.find_num("exitCode"));
            return;
        }
        if (kind == "error") {
            const auto message = ls.find_str("message");
            mcpp::log::warn("provider", message);
            if (!quiet) print_status("Error", message);
            return;
        }
        if (kind != "data" || ls.find_str("dataKind") != "download_progress") return;
        const auto prog = download_progress_of(line);
        if (!prog) return;
        for (const auto& file : prog->files) {
            if (!quiet && !file.sourceHost.empty() && sourceAttempts[file.name] != file.sourceAttempt) {
                sourceAttempts[file.name] = file.sourceAttempt;
                print_status("Source", std::format("{} {} (attempt {})", file.name, file.sourceHost, file.sourceAttempt));
            }
        }
        if (cb) cb(*prog);
        else if (!quiet) {
            std::vector<mcpp::ui::DownloadFile> files;
            for (const auto& f : prog->files)
                files.push_back({f.name, static_cast<std::size_t>(std::max(0.0, f.downloadedBytes)),
                    static_cast<std::size_t>(std::max(0.0, f.totalBytes)), f.started, f.finished});
            download.update(files, prog->elapsedSec);
        }
    };

    inv.options.streams = mcpp::run::Streams::output;
    inv.options.on_line = handle_line;
    inv.options.total = std::chrono::duration_cast<std::chrono::milliseconds>(kDirectInstallTimeout);
    inv.options.idle = std::chrono::duration_cast<std::chrono::milliseconds>(kInterfaceIdleTimeout);
    inv.options.should_stop = [&] { return deadline.expired(); };
    const auto ran = mcpp::run::execute(inv.argv, inv.options);
    const bool idleTimedOut = ran.timed_out;
    const int closeRc = ran.exit_code;
    if (!cb && !quiet) {
        if (!idleTimedOut && resultExitCode == 0 && closeRc == 0) download.finish();
        else download.finish_failed();
    }
    if (idleTimedOut) {
        mcpp::log::warn("xlings", "package install exceeded its progress/process deadline and was stopped");
        if (!quiet) print_status("Network", "Download stopped: no byte progress or process deadline exceeded; check proxy/mirror settings");
        return resultExitCode > 0 ? resultExitCode : 124;
    }
    return (resultExitCode != -1) ? resultExitCode : closeRc;
}

int install_direct(const Env& env, std::string_view target, bool quiet) {
    auto inv = invocation(env, { "install", std::string(target), "-y" });
    mcpp::effects::note_network_access();
    // An empty standard input. Lines are passed through as the inherited
    // terminal showed them before, unless the caller asked for quiet. They are
    // xlings' own progress text, narration: standard error.
    inv.options.streams = quiet ? mcpp::run::Streams::discard : mcpp::run::Streams::output_error_shown;
    inv.options.on_line = [](std::string_view line) { std::println(stderr, "{}", line); };
    inv.options.total = std::chrono::duration_cast<std::chrono::milliseconds>(kDirectInstallTimeout);
    const auto ran = mcpp::run::execute(inv.argv, inv.options);
    const bool timedOut = ran.timed_out;
    const int rc = ran.exit_code;
    if (timedOut)
        mcpp::log::warn("xlings", std::format(
            "`xlings install {}` did not finish within {} hours and was stopped",
            target, kDirectInstallTimeout.count() / 3600));
    return rc;
}

// ─── Sandbox lifecycle ──────────────────────────────────────────────

void seed_xlings_json(const Env& env,
                      std::span<const SeedRepo> repos,
                      std::string_view mirror,
                      const ProjectEnv& penv)
{
    auto path = env.home / ".xlings.json";
    std::string json = "{\n";
    json += "  \"index_repos\": [\n";
    for (std::size_t i = 0; i < repos.size(); ++i) {
        json += std::format("    {{ \"name\": \"{}\", \"url\": \"{}\"",
                            json_escape(repos[i].name),
                            json_escape(repos[i].url));
        if (!repos[i].artifact.empty() && !repos[i].artifactCn.empty())
            json += std::format(", \"artifact\": {{ \"GLOBAL\": \"{}\", \"CN\": \"{}\" }}",
                                json_escape(repos[i].artifact),
                                json_escape(repos[i].artifactCn));
        else if (!repos[i].artifact.empty())
            json += std::format(", \"artifact\": \"{}\"",
                                json_escape(repos[i].artifact));
        if (!repos[i].source.empty())
            json += std::format(", \"source\": \"{}\"",
                                json_escape(repos[i].source));
        if (!repos[i].dir.empty())
            json += std::format(", \"dir\": \"{}\"", json_escape(repos[i].dir.generic_string()));
        if (!repos[i].store.empty())
            json += std::format(", \"store\": \"{}\"", json_escape(repos[i].store.generic_string()));
        json += std::format(" }}{}\n", i + 1 == repos.size() ? "" : ",");
    }
    json += "  ],\n";
    // [xlings] build environment (L-1): materialize deps/workspace/subos/envs
    // verbatim into the keys xlings reads. Each is emitted only when non-empty.
    auto emit_obj = [&](std::string_view key,
                        std::span<const std::pair<std::string,std::string>> kv) {
        json += std::format("  \"{}\": {{\n", key);
        for (std::size_t i = 0; i < kv.size(); ++i)
            json += std::format("    \"{}\": \"{}\"{}\n",
                                json_escape(kv[i].first), json_escape(kv[i].second),
                                i + 1 == kv.size() ? "" : ",");
        json += "  },\n";
    };
    if (!penv.deps.empty()) {
        json += "  \"deps\": [";
        for (std::size_t i = 0; i < penv.deps.size(); ++i)
            json += std::format("{}\"{}\"", i ? ", " : "", json_escape(penv.deps[i]));
        json += "],\n";
    }
    if (!penv.workspace.empty()) emit_obj("workspace", penv.workspace);
    if (!penv.subos.empty())
        json += std::format("  \"subos\": \"{}\",\n", json_escape(penv.subos));
    json += "  \"lang\": \"en\",\n";
    json += std::format("  \"mirror\": \"{}\"\n", json_escape(mirror));
    json += "}\n";
    write_file(path, json);
}

int config_show(const Env& env) {
    std::ifstream file(env.home / ".xlings.json", std::ios::binary);
    if (!file) {
        std::println("mirror = auto (default; registry configuration is not initialized)");
        return 0;
    }
    std::cout << file.rdbuf();
    return 0;
}

int config_set_mirror(const Env& env, std::string_view mirror, bool quiet) {
    if (mirror.empty()) return 0;
    // Configuration recovery must work before the first provider invocation.
    // The adapter owns its format; asking the CLI first would bootstrap through
    // precisely the failed mirror the user is trying to change.
    const auto path = env.home / ".xlings.json";
    auto lock = mcpp::home::FileLock::try_acquire(env.home);
    if (!lock) return 1;
    std::ifstream input(path, std::ios::binary);
    auto doc = nlohmann::json::parse(input, nullptr, false);
    input.close(); // Windows cannot atomically replace a file still held open here.
    if (doc.is_discarded() || !doc.is_object()) return 1;
    if (doc.value("mirror", "") == mirror) return 0;
    doc["mirror"] = mirror;
    auto staged = path;
    staged += ".tmp";
    {
        std::ofstream output(staged, std::ios::binary);
        output << doc.dump(2) << '\n';
        output.close();
        if (!output) return 1;
    }
    std::error_code ec;
    mcpp::home::replace_file(staged, path, ec);
    if (ec) { std::error_code ignored; std::filesystem::remove(staged, ignored); }
    (void)quiet;
    return ec ? 1 : 0;
}

void ensure_init(const Env& env, bool quiet) {
    auto marker = paths::sandbox_init_marker(env);
    if (std::filesystem::exists(marker)) return;

    // Ensure the home directory exists before cd'ing into it.
    std::error_code ec;
    std::filesystem::create_directories(env.home, ec);

    if (!quiet)
        print_status("Initialize", "mcpp sandbox layout (one-time)");
    mcpp::log::ScopedTimer _t_init("init", "sandbox layout (xlings self init)");
    // `self init` initialises the global sandbox, so it runs in global mode
    // whatever the caller's project directory is.
    Env globalEnv = env;
    globalEnv.projectDir.clear();
    auto inv = invocation(globalEnv, { "self", "init" });
    inv.options.streams = mcpp::run::Streams::discard;
    inv.options.total = std::chrono::seconds{120};
    const int rc = mcpp::run::execute(inv.argv, inv.options).exit_code;
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
    mcpp::log::ScopedTimer _t_pe("init", std::format("bootstrap patchelf@{}", pinned::kPatchelfVersion));
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
    auto ninja_name = std::string("ninja") + std::string(mcpplibs::os::exe_suffix);
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
    mcpp::log::ScopedTimer _t_ninja("init", std::format("bootstrap ninja@{}", pinned::kNinjaVersion));
    int rc = install_with_progress(env,
        std::format("xim:ninja@{}", pinned::kNinjaVersion), cb, quiet);
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: failed to bootstrap ninja into mcpp sandbox (exit {})",
            rc);
    }
}

namespace {

// "NASM version 2.16.03 compiled on ..." → true iff ≥ 2.16. A binary that
// fails to run or reports something unparseable is unusable — false.
bool nasm_version_ok(const std::filesystem::path& bin) {
    auto r = mcpp::run::capture_exec({bin.string(), "-v"});
    if (r.exit_code != 0) return false;
    auto pos = r.output.find("version ");
    if (pos == std::string::npos) return false;
    int major = 0, minor = 0;
    const char* p = r.output.c_str() + pos + 8;
    while (*p >= '0' && *p <= '9') major = major * 10 + (*p++ - '0');
    if (*p++ != '.') return false;
    while (*p >= '0' && *p <= '9') minor = minor * 10 + (*p++ - '0');
    return major > 2 || (major == 2 && minor >= 16);
}

} // namespace

std::optional<std::filesystem::path> find_sandbox_nasm(const Env& env) {
    auto root = paths::xim_tool_root(env, "nasm");
    auto nasm_name = std::string("nasm") + std::string(mcpplibs::os::exe_suffix);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;
    for (auto& v : std::filesystem::directory_iterator(root, ec)) {
        for (auto cand : { v.path() / nasm_name, v.path() / "bin" / nasm_name }) {
            if (std::filesystem::exists(cand, ec) && nasm_version_ok(cand))
                return cand;
        }
    }
    return std::nullopt;
}

// THE ECOSYSTEM COPY IS TRIED FIRST, AND IT USED TO BE TRIED SECOND.
//
// This function answered `which("nasm")` before looking in the sandbox, so a
// machine with an assembler on PATH assembled with that one and a machine
// without downloaded the pinned `xim:nasm`. Three machines could produce three
// different objects from one source tree with nothing in the build saying so,
// which is the property every other tool in this engine is arranged to avoid:
// the compiler, the linker, ninja and patchelf all come from the graph or the
// sandbox, and none of them asks PATH first.
//
// The host copy is kept, and only as a last resort: a machine that is offline
// and already has a usable assembler can still build, which is the one case
// the sandbox route cannot serve. When it is the one used, the caller says so
// in the build report rather than leaving it silent --- `nasm_is_from_host`
// below is how the caller tells the two apart.
std::optional<std::filesystem::path> find_usable_nasm(const Env& env) {
    if (auto sandboxed = find_sandbox_nasm(env)) return sandboxed;
    auto nasm_name = std::string("nasm") + std::string(mcpplibs::os::exe_suffix);
    if (auto sys = mcpp::invocation::which(nasm_name);
        sys && nasm_version_ok(*sys)) {
        return sys;
    }
    return std::nullopt;
}

// Whether a path `find_usable_nasm` returned is the host's rather than the
// sandbox's. Asked by the caller so that a host tool reaching a build is
// NAMED there; a host tool that reaches a build silently is the defect, not
// the host tool.
bool nasm_is_from_host(const Env& env, const std::filesystem::path& bin) {
    auto root = paths::xim_tool_root(env, "nasm");
    std::error_code ec;
    auto canonRoot = std::filesystem::weakly_canonical(root, ec);
    auto canonBin  = std::filesystem::weakly_canonical(bin, ec);
    if (canonRoot.empty() || canonBin.empty()) return true;
    auto r = canonRoot.string();
    auto b = canonBin.string();
    return b.rfind(r, 0) != 0;
}

// ─── Index freshness ────────────────────────────────────────────────

bool is_index_fresh(const Env& env, std::int64_t ttlSeconds) {
    return is_index_dir_fresh(default_index_dir(env), ttlSeconds);
}

bool is_official_index_fresh(const Env& env, std::int64_t ttlSeconds) {
    return is_index_dir_fresh(official_index_dir(env), ttlSeconds);
}

IndexStatus default_index_status(const Env& env, std::int64_t ttlSeconds) {
    return index_status_for(default_index_dir(env), ttlSeconds);
}

IndexStatus official_index_status(const Env& env, std::int64_t ttlSeconds) {
    return index_status_for(official_index_dir(env), ttlSeconds);
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

// The one entry point every refresh in this process passes through.
//
//   an index-side change must never take mcpp from "works" to "does not work"
//
// A published index can raise its client-version floor (index.toml
// min_mcpp). An index mcpp owns (one whose data directory mcpp declared,
// design 2026-10-16 §6) is therefore fetched into a staging directory beside
// it (xlings' `sync_index` with a destination, X1), judged there, and swapped
// in only when this mcpp can read it -- or when the copy in use is no better.
// The rest (the provider's own indexes, which carry no floor of mcpp's) are
// refreshed by the provider in place; it leaves an owned index that is there
// to its owner.
namespace {
std::atomic<long long> g_index_refresh_timeout_s{kDefaultIndexRefreshTimeout.count()};

struct StagedOutcome {
    std::vector<std::filesystem::path> kept;        // the refreshed copy needs a newer mcpp; the old one stays
    std::vector<std::filesystem::path> unusable;    // no copy this mcpp can read
    std::map<std::filesystem::path, std::string> requiredMcpp;
};

// One owned index: <dir>.staging-<n> fetched, judged, then exchanged with
// <dir> (or renamed onto it when there is none yet); the staging directory
// holds the old copy afterwards and is removed.
int refresh_owned(const Env& env, const paths::DeclaredIndex& d, StagedOutcome& out) {
    std::error_code ec;
    auto staging = d.dir;
    staging += std::format(".staging-{:x}", std::random_device{}());
    std::filesystem::create_directories(d.dir.parent_path(), ec);
    const auto args = nlohmann::json{{"name", d.name}, {"dest", staging.generic_string()}}.dump();
    auto r = call(env, "sync_index", args, nullptr);
    if (!r || r->exitCode != 0 || !std::filesystem::is_directory(staging, ec)) {
        std::filesystem::remove_all(staging, ec);
        return r ? (r->exitCode != 0 ? r->exitCode : 1) : 1;
    }
    const bool present = std::filesystem::exists(d.dir, ec);
    const bool before = present && mcpp::pm::index_usable(d.dir);
    if (!mcpp::pm::index_usable(staging)) {
        if (auto c = mcpp::pm::read_index_contract(staging)) out.requiredMcpp[d.dir] = c->minMcpp;
        if (before) {
            std::filesystem::remove_all(staging, ec);
            out.kept.push_back(d.dir);
            return 0;
        }
        out.unusable.push_back(d.dir);
    }
    auto swapped = present ? mcpplibs::os::fs::exchange(staging, d.dir)
                           : mcpplibs::os::fs::place(staging, d.dir);
    std::filesystem::remove_all(staging, ec);
    if (!swapped) {
        mcpp::log::warn("index", std::format("the refreshed index `{}` could not replace the one in use: {}",
                                             d.name, swapped.error().message()));
        return 1;
    }
    mark_index_refreshed(d.dir);
    return 0;
}

// The raw sync. Everything that makes a refresh SAFE lives in the wrapper
// below; this function's only job is to run the provider's refresh and report.
int update_index_unguarded(const Env& env, bool quiet);
} // namespace

void set_index_refresh_timeout(std::chrono::seconds timeout) {
    g_index_refresh_timeout_s.store(
        timeout.count() > 0 ? timeout.count() : kDefaultIndexRefreshTimeout.count(),
        std::memory_order_relaxed);
}

std::chrono::seconds index_refresh_timeout() {
    return std::chrono::seconds{g_index_refresh_timeout_s.load(std::memory_order_relaxed)};
}

int update_index(const Env& env, bool quiet) {
    return update_index_unguarded(env, quiet);
}

namespace {
int update_index_unguarded(const Env& env, bool quiet) {
    // Offline is absolute: no caller gets to reach the network by going around
    // the decision layer. Reported as success so a build that can still resolve
    // everything locally proceeds — the caller that genuinely needed the data
    // fails on its own missing answer, with a message that says so.
    if (mcpp::effects::offline_mode()) {
        mcpp::log::verbose("index", "offline mode: skipping index update");
        return 0;
    }

    // Cross-process mutex. The sync rewrites a tree several concurrent mcpp
    // processes read (parallel CI jobs on one MCPP_HOME, two terminals, a
    // workspace fan-out), and it also writes the refresh markers. The BMI cache
    // has had this guard for a while; the index never did.
    //
    // NON-BLOCKING BY DESIGN: whoever holds the lock is already doing the exact
    // work we wanted, so waiting buys nothing and a queue of stalled builds is
    // precisely the symptom this whole change exists to remove. Skipping is
    // also why this must not be reported as failure.
    std::error_code lockEc;
    std::filesystem::create_directories(paths::index_data(env), lockEc);
    auto lock = mcpp::home::FileLock::try_acquire(paths::index_data(env));
    if (!lock) {
        mcpp::log::verbose("index",
            "another process is refreshing the index — skipping this one");
        return 0;
    }

    // The indices mcpp owns, each staged and judged before it replaces the
    // copy in use. A floor is not an error of the run (an index is data;
    // mcpp is the program): each case is one closing notice, printed after
    // the command's own output, and only because this run refreshed.
    StagedOutcome staged;
    int ownedRc = 0;
    for (const auto& d : paths::declared_indices(env))
        if (d.owned) ownedRc = std::max(ownedRc, refresh_owned(env, d, staged));
    auto advice = [&](const std::filesystem::path& dir) {
        auto it = staged.requiredMcpp.find(dir);
        return mcpp::pm::index_floor_upgrade_advice(
            it == staged.requiredMcpp.end() ? std::string_view{} : std::string_view{it->second});
    };
    for (auto& dir : staged.kept)
        mcpp::ui::add_closing_notice("MCPP_INDEX_REQUIRES_NEWER_MCPP", std::format(
            "the refreshed package index `{}` requires a newer mcpp; this run "
            "used the previous index. {}", dir.filename().string(), advice(dir)));
    for (auto& dir : staged.unusable)
        mcpp::ui::add_closing_notice("MCPP_INDEX_REQUIRES_NEWER_MCPP", std::format(
            "the package index `{}` requires a newer mcpp and no earlier copy is "
            "usable; packages it serves cannot be resolved. {}", dir.filename().string(), advice(dir)));

    // Through the NDJSON interface, as installs are (W11): the refresh reports
    // its steps as events and is drawn by the same renderer, instead of the
    // bare CLI's terminal text, which was either reprinted verbatim or, for
    // the automatic refresh, discarded, so a refresh of many seconds showed
    // nothing. The interface's `update_packages` and the CLI's `update` are
    // one function in xlings (xim::cmd_update).
    auto inv = invocation(env, { "interface", "update_packages", "--args", "{}" });
    inv.options.streams = mcpp::run::Streams::output;
    inv.options.total = std::chrono::duration_cast<std::chrono::milliseconds>(index_refresh_timeout());
    mcpp::effects::note_network_access();
    // The index sync is a network git operation; a single transient blip (DNS,
    // TLS reset, a mirror hiccup) otherwise fails a cold `mcpp self env` /
    // first-run init outright (e.g. CI's index/sandbox bootstrap). Retry with
    // linear backoff. The success path returns on the FIRST attempt — zero
    // added latency in steady state; only a genuine failure pays the backoff.
    constexpr int kMaxAttempts = 3;
    int rc = 0;
    const auto refreshBound = index_refresh_timeout();
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        bool timedOut = false;
        {
            IndexRefreshRenderer renderer;
            inv.options.on_line = [&renderer](std::string_view line) { renderer.line(line); };
            const auto ran = mcpp::run::execute(inv.argv, inv.options);
            rc = ran.exit_code;
            timedOut = ran.timed_out;
            if (rc == 0 && renderer.result_exit() > 0) rc = renderer.result_exit();
            if (rc != 0 || timedOut) renderer.fail();
        }
        if (rc == 0 && !timedOut) { mark_known_indexes_refreshed(env); return ownedRc; }
        // A refresh that exceeded its bound is not retried: the retries exist for
        // a transient failure that ends, and a connection that never answers
        // would only be waited on three times. The caller treats the refresh as
        // failed and resolves from the local index, as it does for any failure.
        if (timedOut) {
            std::println(stderr,
                "warning: the package index refresh did not finish within {} seconds and was "
                "stopped; continuing with the local index "
                "(the bound is [index] refresh_timeout in mcpp's config.toml)",
                refreshBound.count());
            return rc != 0 ? rc : 124;
        }
        if (attempt < kMaxAttempts) {
            int delay = attempt * 2;  // 2s, then 4s
            mcpp::log::verbose("index", std::format(
                "index update attempt {}/{} failed (rc {}); retrying in {}s",
                attempt, kMaxAttempts, rc, delay));
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        }
    }
    // Said, not only logged, to a caller that refreshes on its own (`quiet`:
    // the TTL refresh before a build), which continues with the local index;
    // `mcpp index update` reports the failure itself.
    if (quiet)
        std::println(stderr,
            "warning: the package index refresh failed after {} attempts (exit {}); "
            "continuing with the local index", kMaxAttempts, rc);
    return rc;
}
} // namespace

void ensure_index_fresh(const Env& env, std::int64_t ttlSeconds, bool quiet) {
    if (is_index_fresh(env, ttlSeconds)) return;
    if (!quiet)
        print_status("Updating", "package index (auto-refresh)");
    update_index(env, /*quiet=*/true);
}

bool official_package_present(const Env& env, std::string_view packageName) {
    auto pkg = official_package_file(env, packageName);
    std::error_code ec;
    return !pkg.empty() && std::filesystem::exists(pkg, ec);
}

} // namespace mcpp::provider::xlings
