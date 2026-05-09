// mcpp.config — global config + paths + xlings binary acquisition.
//
// Layout (per docs/14-data-layout.md):
//   $MCPP_HOME/                 default ~/.mcpp/
//     bin/mcpp                  mcpp binary (self-contained mode)
//     registry/                 XLINGS_HOME for mcpp's xlings
//       bin/xlings              vendored xlings binary (= <XLINGS_HOME>/bin/xlings)
//       .xlings.json            seeded with index_repos = [mcpp-index]
//     bmi/<fp>/                 BMI cache (existing)
//     cache/                    metadata caches
//     config.toml               this module's input
//
// Initialization is silent and idempotent: every Config::load() ensures
// the directory tree exists and seeds .xlings.json if missing.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.config;

import std;
import mcpp.libs.toml;

export namespace mcpp::config {

inline constexpr std::string_view kXlingsPinnedVersion = "0.4.7";

struct IndexRepo {
    std::string name;
    std::string url;
};

struct GlobalConfig {
    // Resolved paths
    std::filesystem::path           mcppHome;            // ~/.mcpp/
    std::filesystem::path           binDir;              // mcppHome/bin
    std::filesystem::path           xlingsBinary;        // mcppHome/registry/bin/xlings
    std::filesystem::path           registryDir;         // mcppHome/registry
    std::filesystem::path           bmiCacheDir;         // mcppHome/bmi
    std::filesystem::path           metaCacheDir;        // mcppHome/cache
    std::filesystem::path           logDir;              // mcppHome/log
    std::filesystem::path           configFile;          // mcppHome/config.toml

    // From config.toml [xlings]
    std::string                     xlingsBinaryMode;    // "bundled" | "system" | absolute path
    std::filesystem::path           xlingsHomeOverride;  // empty = use registryDir

    // From config.toml [index]
    std::string                     defaultIndex;        // "mcpp-index"
    std::vector<IndexRepo>          indexRepos;

    // From config.toml [cache]
    std::int64_t                    searchTtlSeconds = 3600;

    // From config.toml [build]
    std::int64_t                    defaultJobs = 0;
    std::string                     defaultBackend = "ninja";

    // From config.toml [toolchain] (M5.5)
    //   default = "<compiler>@<version>"   e.g. "gcc@15.1.0"
    // Empty means no global default; mcpp will hard-error unless the project
    // mcpp.toml declares its own [toolchain].
    std::string                     defaultToolchain;

    // Resolved xlings home (registryDir unless overridden)
    std::filesystem::path xlingsHome() const {
        return xlingsHomeOverride.empty() ? registryDir : xlingsHomeOverride;
    }
};

struct ConfigError { std::string message; };

// Load (or create) the global config. Idempotent. Performs:
//   1. Resolve $MCPP_HOME (env or ~/.mcpp)
//   2. Create directory tree if missing
//   3. Seed config.toml if missing
//   4. Seed registry/.xlings.json if missing (so xlings won't auto-add awesome)
//   5. Acquire xlings binary if missing (system copy → release download → fail)

// Streaming download-progress info for the sandbox bootstrap step (patchelf,
// ninja). cli.cppm wraps a ui::ProgressBar around this so the user sees the
// same percent / MB / s display they get for `mcpp toolchain install`.
//
// We can't use mcpp.fetcher's EventHandler here because fetcher imports
// config — the dependency would be cyclic.
struct BootstrapFile {
    std::string  name;             // xim package id, e.g. "xim:patchelf@0.18.0"
    double       downloadedBytes = 0;
    double       totalBytes      = 0;
    bool         started         = false;
    bool         finished        = false;
};

// One xlings download_progress event. Carries the full `files[]` array
// so the consumer can decide which one to display: a multi-package
// install (xim:gcc with its glibc / binutils / linux-headers / runtime
// deps) reshuffles which file occupies slot 0 across events, and the
// caller that only saw the first slot would re-emit the static
// "Downloading <pkg>" line every time the order shifted.
struct BootstrapProgress {
    std::vector<BootstrapFile>  files;
    double                      elapsedSec = 0;
};
using BootstrapProgressCallback = std::function<void(const BootstrapProgress&)>;

std::expected<GlobalConfig, ConfigError> load_or_init(
    bool quiet = false,
    BootstrapProgressCallback onBootstrapProgress = {});

// Pretty-print resolved config for `mcpp env` command.
void print_env(const GlobalConfig& cfg);

// M5.5: persist [toolchain].default to config.toml.
std::expected<void, ConfigError>
write_default_toolchain(const GlobalConfig& cfg, std::string_view spec);

} // namespace mcpp::config

namespace mcpp::config {

namespace t = mcpp::libs::toml;

namespace {

// Resolve MCPP_HOME, in priority order:
//   1. $MCPP_HOME env var (explicit override — used by CI / dev / multi-instance)
//   2. <binary-dir>/.. — self-contained mode, when mcpp lives at
//      <root>/bin/mcpp. Release tarballs and `xlings install mcpp`
//      use this layout; the unpacked tree IS the home. Dev builds
//      live under target/<triple>/<fp>/bin/mcpp, which is the same
//      "in a bin/ dir" shape — so we additionally exclude any path
//      with a "target" ancestor as mcpp's own dev convention.
//   3. fallback to $HOME/.mcpp.
// Right-pad a verb to 12 columns, matching mcpp::ui::verb_padded so the
// bootstrap status lines line up under the cyan "Downloading …" lines
// produced via the BootstrapProgressCallback. We can't import mcpp.ui
// from here (cyclic dep), so this is a tiny duplicate of that helper —
// no color, no fanciness.
void print_status(std::string_view verb, std::string_view msg) {
    constexpr std::size_t W = 12;
    if (verb.size() >= W) {
        std::println("{} {}", verb, msg);
    } else {
        std::println("{}{} {}", std::string(W - verb.size(), ' '), verb, msg);
    }
}

std::filesystem::path home_dir() {
    if (auto* e = std::getenv("MCPP_HOME"); e && *e)
        return std::filesystem::path(e);

    std::error_code ec;
    auto exe = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec && exe.parent_path().filename() == "bin") {
        // Dev builds emit binaries at target/<triple>/<fp>/bin/<exe>,
        // matching the bin/ shape. Any ancestor literally named
        // "target" disqualifies self-contained mode and falls through
        // to $HOME/.mcpp — so first-run on a dev binary doesn't drop
        // a half-populated sandbox into target/.
        bool isDevPath = false;
        for (auto p = exe.parent_path();
             !p.empty() && p != p.parent_path();
             p = p.parent_path()) {
            if (p.filename() == "target") { isDevPath = true; break; }
        }
        if (!isDevPath)
            return exe.parent_path().parent_path();
    }

    if (auto* e = std::getenv("HOME"); e && *e)
        return std::filesystem::path(e) / ".mcpp";
    return std::filesystem::current_path() / ".mcpp";
}

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

void write_file(const std::filesystem::path& p, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream os(p);
    os << content;
}

bool write_default_config_toml(const std::filesystem::path& path) {
    constexpr auto tmpl = R"(# mcpp global config — auto-generated; safe to edit.

[xlings]
# binary: "bundled" (use $MCPP_HOME/registry/bin/xlings) | "system" | absolute path
binary = "bundled"
# home:   empty = use $MCPP_HOME/registry; can override
home   = ""

[index]
default = "mcpp-index"

[index.repos."mcpp-index"]
url = "https://github.com/mcpp-community/mcpp-index.git"
# xlings auto-adds xim / awesome / scode / d2x as defaults.

[cache]
search_ttl_seconds = 3600

[build]
default_jobs    = 0
default_backend = "ninja"
)";
    write_file(path, tmpl);
    return std::filesystem::exists(path);
}

bool write_default_xlings_json(const std::filesystem::path& path,
                               const std::vector<IndexRepo>& repos)
{
    std::string json = "{\n";
    json += "  \"index_repos\": [\n";
    for (std::size_t i = 0; i < repos.size(); ++i) {
        json += std::format("    {{ \"name\": \"{}\", \"url\": \"{}\" }}{}\n",
                            repos[i].name, repos[i].url,
                            i + 1 == repos.size() ? "" : ",");
    }
    json += "  ],\n";
    json += "  \"lang\": \"en\",\n";
    json += "  \"mirror\": \"\"\n";
    json += "}\n";
    write_file(path, json);
    return std::filesystem::exists(path);
}

// Try to acquire xlings binary. Returns the path if successful.
std::expected<std::filesystem::path, std::string>
acquire_xlings_binary(const std::filesystem::path& destBin, bool quiet)
{
    if (std::filesystem::exists(destBin)) return destBin;

    std::error_code ec;
    std::filesystem::create_directories(destBin.parent_path(), ec);

    // 1. Explicit override
    if (auto* e = std::getenv("MCPP_VENDORED_XLINGS"); e && *e) {
        std::filesystem::path src{e};
        if (std::filesystem::exists(src)) {
            std::filesystem::copy_file(src, destBin,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                std::filesystem::permissions(destBin,
                    std::filesystem::perms::owner_exec
                  | std::filesystem::perms::group_exec
                  | std::filesystem::perms::others_exec,
                  std::filesystem::perm_options::add, ec);
                if (!quiet) print_status("Bundled",
                    std::format("xlings v{} (from MCPP_VENDORED_XLINGS)", kXlingsPinnedVersion));
                return destBin;
            }
        }
    }

    // 2. Copy from system (`which xlings`)
    auto sys = run_capture("command -v xlings 2>/dev/null");
    if (sys) {
        std::string p = *sys;
        while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
        if (!p.empty() && std::filesystem::exists(p)) {
            std::filesystem::copy_file(p, destBin,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                std::filesystem::permissions(destBin,
                    std::filesystem::perms::owner_exec
                  | std::filesystem::perms::group_exec
                  | std::filesystem::perms::others_exec,
                  std::filesystem::perm_options::add, ec);
                if (!quiet) print_status("Bundled",
                    std::format("xlings (copied from system: {})", p));
                return destBin;
            }
        }
    }

    // 3. Download from GitHub Release (placeholder — real impl uses curl)
    // We delegate to curl/wget in the bash bootstrap; for in-process robustness
    // we just instruct the user.
    return std::unexpected(std::format(
        "xlings binary not found. Either:\n"
        "  - install via: curl -fsSL https://raw.githubusercontent.com/d2learn/xlings/refs/heads/main/tools/other/quick_install.sh | bash\n"
        "  - export MCPP_VENDORED_XLINGS=/abs/path/to/xlings\n"
        "  - set [xlings].binary = \"system\" in {}",
        (destBin.parent_path().parent_path() / "config.toml").string()));
}

// Run `xlings self init` against the sandbox to create the standard
// directory layout (subos/default/{bin,lib,usr,generations}, data/, config/,
// shell profiles, and the empty workspace .xlings.json). Idempotent.
//
// TODO(xlings-upstream): Once xlings ships a `xlings sandbox bootstrap` /
// `xlings self init --copy-self-bin` API that does init + binary placement
// + patchelf in one shot, this function and ensure_sandbox_xlings_binary +
// ensure_sandbox_patchelf below can collapse into a single call.
void ensure_sandbox_init(const GlobalConfig& cfg, bool quiet) noexcept {
    // Marker: the sandbox layout is "complete enough" if
    // <home>/subos/default/.xlings.json exists. xlings self init creates it.
    auto marker = cfg.xlingsHome() / "subos" / "default" / ".xlings.json";
    if (std::filesystem::exists(marker)) return;

    if (!quiet)
        print_status("Initialize", "mcpp sandbox layout (one-time)");
    auto cmd = std::format(
        "cd '{}' && env -u XLINGS_PROJECT_DIR XLINGS_HOME='{}' '{}' self init >/dev/null 2>&1",
        cfg.xlingsHome().string(),
        cfg.xlingsHome().string(),
        cfg.xlingsBinary.string());
    int rc = std::system(cmd.c_str());
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: `xlings self init` failed for sandbox at '{}'",
            cfg.xlingsHome().string());
    }
}

// With the 0.0.4 layout change (xlings binary at <MCPP_HOME>/registry/bin/
// = <XLINGS_HOME>/bin/), the bundled xlings IS already at the path xlings's
// shim-creation guard checks (`paths.homeDir / "bin" / "xlings"`).
// No mirroring / hardlinking needed — this function is now a no-op.
void ensure_sandbox_xlings_binary(const GlobalConfig& /*cfg*/, bool /*quiet*/) noexcept {
    // Intentional no-op: xlingsBinary == xlingsHome()/bin/xlings.
}

// ─── Bootstrap install: shared event-stream parser + driver ────────────
//
// Both ensure_sandbox_patchelf and ensure_sandbox_ninja go through xlings's
// `interface install_packages --args '...'` JSON-event pipe and stream the
// `download_progress` events into a caller-supplied progress callback. We
// can't use mcpp.fetcher here (cyclic import) so it's a small custom parser.

// Pull a numeric/bool/string field out of a flat JSON region. Cheap; no
// nested array/object handling — the keys we extract are all leaves.
struct LineScan {
    std::string_view s;
    static bool starts_with(std::string_view a, std::string_view b) {
        return a.size() >= b.size() && a.compare(0, b.size(), b) == 0;
    }
    std::string find_str(std::string_view key) const {
        std::string n = std::format("\"{}\":\"", key);
        auto p = s.find(n);
        if (p == std::string_view::npos) return "";
        p += n.size();
        std::string out;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) { out.push_back(s[p+1]); p += 2; continue; }
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

// Run xlings with its NDJSON `interface install_packages` capability and
// drive the progress callback off the stream. Returns the install exit
// code (0 on success).
int run_xim_install_with_progress(const GlobalConfig& cfg,
                                  std::string_view ximTarget,
                                  const BootstrapProgressCallback& cb)
{
    // The args JSON is fixed-shape so we hand-format rather than pulling
    // in a JSON writer.
    auto argsJson = std::format(
        R"({{"targets":["{}"],"yes":true}})", ximTarget);

    // Shell-escape via single quotes; the args JSON itself contains no
    // single quotes so this is safe.
    auto cmd = std::format(
        "cd '{}' && env -u XLINGS_PROJECT_DIR XLINGS_HOME='{}' '{}' interface install_packages --args '{}' 2>/dev/null",
        cfg.xlingsHome().string(),
        cfg.xlingsHome().string(),
        cfg.xlingsBinary.string(),
        argsJson);

    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return -1;

    std::array<char, 16384> buf{};
    std::string acc;
    int          resultExitCode = -1;

    auto handle_line = [&](std::string_view line) {
        // Only two event kinds drive UX:
        //   data + dataKind=download_progress  → progress callback
        //   result                              → final exit code
        LineScan ls{line};
        auto kind = ls.find_str("kind");
        if (kind == "result") {
            resultExitCode = static_cast<int>(ls.find_num("exitCode"));
            return;
        }
        if (kind != "data") return;
        if (ls.find_str("dataKind") != "download_progress") return;
        if (!cb) return;

        // Walk every entry in "files":[ {...}, {...}, ... ] and pass them
        // all up. xlings reorders the array as files start/finish during
        // multi-package installs (gcc bundles glibc + binutils + headers
        // + runtime); a consumer that only read slot 0 would flicker
        // between names and re-emit the static "Downloading <pkg>" line
        // each event.
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

// Ensure ninja is installed in the sandbox (real binary at
// <sandbox>/data/xpkgs/xim-x-ninja/<v>/bin/ninja). mcpp's ninja_backend
// uses this absolute path to spawn ninja, sidestepping the system xlings
// ninja shim (which requires per-tool version activation that isn't
// guaranteed in CI / fresh user setups).
//
// TODO(xlings-upstream): once xlings 0.4.10's xvm shim creation is
// guaranteed to work cross-sandbox without requiring the
// ensure_sandbox_xlings_binary hardlink + xlings self init dance, the
// `<sandbox>/subos/default/bin/ninja` shim would also work and we
// could drop this and just rely on PATH.
void ensure_sandbox_ninja(const GlobalConfig& cfg, bool quiet,
                          const BootstrapProgressCallback& cb) noexcept
{
    auto root = cfg.xlingsHome() / "data" / "xpkgs" / "xim-x-ninja";
    if (std::filesystem::exists(root)) {
        std::error_code ec;
        for (auto& v : std::filesystem::directory_iterator(root, ec)) {
            // xim's ninja xpkg places the binary at <v>/ninja (no bin/ subdir).
            if (std::filesystem::exists(v.path() / "ninja")) return;
        }
    }
    if (!quiet)
        print_status("Bootstrap", "ninja into mcpp sandbox (one-time)");
    int rc = run_xim_install_with_progress(cfg, "xim:ninja@1.12.1", cb);
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: failed to bootstrap ninja into mcpp sandbox (exit {})",
            rc);
    }
}

// xim packages (gcc / binutils / openssl / d2x / ...) call `elfpatch.auto`
// in their install() hooks to rewrite ELF binaries' PT_INTERP / RUNPATH to
// sandbox-local glibc paths. Internally that goes through xmake's
// `find_tool("patchelf")`. If patchelf is not present, the patcher logs a
// warning and silently no-ops (xim-pkgindex/elfpatch.lua:304-308):
//
//     if not patch_tool then
//         log.warn("patchelf not found, skip patching")
//         return result
//     end
//
// Without ELF patching, every xim-built binary keeps its build-host
// hardcoded loader path (`/home/xlings/.xlings_data/.../ld-linux-x86-64.so.2`),
// which doesn't exist on a fresh user/CI machine — produced binaries fail
// to exec ("cannot execute: required file not found").
//
// Fix: when initializing mcpp's private xlings sandbox for the first time,
// have the bundled xlings install patchelf into the same sandbox. xim's
// elfpatch.auto then finds it via xvm + PATH (set up by Fetcher).
//
// patchelf's own install hook is a plain extract — it does NOT call
// elfpatch.auto, so there's no chicken-and-egg.
void ensure_sandbox_patchelf(const GlobalConfig& cfg, bool quiet,
                              const BootstrapProgressCallback& cb) noexcept
{
    auto marker = cfg.xlingsHome() / "data" / "xpkgs"
                / "xim-x-patchelf" / "0.18.0" / "bin" / "patchelf";
    if (std::filesystem::exists(marker)) return;

    if (!quiet)
        print_status("Bootstrap", "patchelf into mcpp sandbox (one-time)");
    int rc = run_xim_install_with_progress(cfg, "xim:patchelf@0.18.0", cb);
    if (rc != 0 && !quiet) {
        std::println(stderr,
            "warning: failed to bootstrap patchelf into mcpp sandbox; "
            "subsequent xim installs may skip ELF rewriting");
    }
}

} // namespace

std::expected<GlobalConfig, ConfigError> load_or_init(
    bool quiet,
    BootstrapProgressCallback onBootstrapProgress)
{
    GlobalConfig cfg;

    // 1. Resolve paths
    cfg.mcppHome      = home_dir();
    cfg.binDir        = cfg.mcppHome / "bin";
    cfg.registryDir   = cfg.mcppHome / "registry";
    // xlings lives under registry/, not bin/ — it's a registry tool,
    // not a user-facing binary. This also places it exactly at
    // <XLINGS_HOME>/bin/xlings, which satisfies xlings's own shim-
    // creation guard (`if fs::exists(homeDir/"bin"/"xlings")`),
    // making ensure_sandbox_xlings_binary() a no-op.
    cfg.xlingsBinary  = cfg.registryDir / "bin" / "xlings";
    cfg.bmiCacheDir   = cfg.mcppHome / "bmi";
    cfg.metaCacheDir  = cfg.mcppHome / "cache";
    cfg.logDir        = cfg.mcppHome / "log";
    cfg.configFile    = cfg.mcppHome / "config.toml";

    // 2. Create directory tree
    std::error_code ec;
    for (auto& d : { cfg.binDir, cfg.registryDir, cfg.bmiCacheDir,
                     cfg.metaCacheDir, cfg.logDir }) {
        std::filesystem::create_directories(d, ec);
        if (ec) return std::unexpected(ConfigError{
            std::format("cannot create '{}': {}", d.string(), ec.message())});
    }

    // 3. Seed config.toml if missing
    bool fresh_config = !std::filesystem::exists(cfg.configFile);
    if (fresh_config) write_default_config_toml(cfg.configFile);

    // 4. Load config.toml
    auto doc = t::parse_file(cfg.configFile);
    if (!doc) {
        return std::unexpected(ConfigError{
            std::format("invalid config.toml: {}", doc.error().message)});
    }

    cfg.xlingsBinaryMode = doc->get_string("xlings.binary").value_or("bundled");
    if (auto h = doc->get_string("xlings.home"); h && !h->empty())
        cfg.xlingsHomeOverride = *h;
    cfg.defaultIndex   = doc->get_string("index.default").value_or("mcpp-index");
    cfg.searchTtlSeconds = doc->get_int("cache.search_ttl_seconds").value_or(3600);
    cfg.defaultJobs    = doc->get_int("build.default_jobs").value_or(0);
    cfg.defaultBackend = doc->get_string("build.default_backend").value_or("ninja");
    cfg.defaultToolchain = doc->get_string("toolchain.default").value_or("");

    // [index.repos.NAME] tables
    if (auto* repos = doc->get_table("index.repos")) {
        for (auto& [name, val] : *repos) {
            if (!val.is_table()) continue;
            auto& tt = val.as_table();
            auto it = tt.find("url");
            if (it == tt.end() || !it->second.is_string()) continue;
            cfg.indexRepos.push_back({ name, it->second.as_string() });
        }
    }
    // Defaults: only mcpp-index. xlings auto-adds its own standard
    // defaults (xim / awesome / scode / d2x) because globalIndexRepos_
    // is non-empty (per xlings/src/core/config.cppm). Explicitly listing
    // them ourselves can cause cross-index name conflicts during
    // dependency resolution (e.g. linux-headers existing in both
    // scode and xim). See docs/21 §VII.
    auto add_default = [&](std::string_view name, std::string_view url) {
        for (auto& r : cfg.indexRepos) if (r.name == name) return;
        cfg.indexRepos.push_back({ std::string(name), std::string(url) });
    };
    add_default("mcpp-index", "https://github.com/mcpp-community/mcpp-index.git");

    // 5. Seed registry/.xlings.json if missing
    auto xjson = cfg.xlingsHome() / ".xlings.json";
    if (!std::filesystem::exists(xjson)) {
        write_default_xlings_json(xjson, cfg.indexRepos);
    }

    // 6. Acquire xlings binary if needed
    if (cfg.xlingsBinaryMode == "bundled") {
        auto xbin = acquire_xlings_binary(cfg.xlingsBinary, quiet);
        if (!xbin) return std::unexpected(ConfigError{xbin.error()});
    } else if (cfg.xlingsBinaryMode == "system") {
        auto sys = run_capture("command -v xlings 2>/dev/null");
        if (!sys || sys->empty())
            return std::unexpected(ConfigError{"system xlings not found in PATH"});
        std::string p = *sys;
        while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
        cfg.xlingsBinary = p;
    } else {
        cfg.xlingsBinary = cfg.xlingsBinaryMode;
        if (!std::filesystem::exists(cfg.xlingsBinary))
            return std::unexpected(ConfigError{std::format(
                "configured xlings binary not found: {}", cfg.xlingsBinary.string())});
    }

    // 7. Sandbox bootstrap (mcpp self-contained xlings environment).
    //    Order matters:
    //      a. Mirror xlings binary into sandbox so shim creation works.
    //      b. xlings self init: creates subos/default/{bin,lib,usr} skeleton.
    //      c. Install patchelf so xim install hooks can patch ELF binaries.
    //
    //    TODO(xlings-upstream): collapse into a single
    //    `xlings sandbox bootstrap --home <X>` once that command exists
    //    upstream (see docs/short-term-vs-long-track plan).
    ensure_sandbox_xlings_binary(cfg, quiet);
    ensure_sandbox_init(cfg, quiet);
    ensure_sandbox_patchelf(cfg, quiet, onBootstrapProgress);
    ensure_sandbox_ninja(cfg, quiet, onBootstrapProgress);

    return cfg;
}

void print_env(const GlobalConfig& cfg) {
    std::println("MCPP_HOME           = {}", cfg.mcppHome.string());
    std::println("xlings binary       = {}", cfg.xlingsBinary.string());
    std::println("xlings home         = {}", cfg.xlingsHome().string());
    std::println("config              = {}", cfg.configFile.string());
    std::println("BMI cache           = {}", cfg.bmiCacheDir.string());
    std::println("meta cache          = {}", cfg.metaCacheDir.string());
    if (!cfg.defaultToolchain.empty())
        std::println("default toolchain   = {}", cfg.defaultToolchain);
    else
        std::println("default toolchain   = (none — run `mcpp toolchain install gcc 16.1.0`)");
    std::println("");
    std::println("Index repos:");
    for (auto& r : cfg.indexRepos) {
        bool isDefault = (r.name == cfg.defaultIndex);
        std::println("  {} {}{}",
                     r.name, r.url, isDefault ? "  (default)" : "");
    }
}

// M5.5: persist [toolchain].default into config.toml without disturbing
// other fields. Naive: read text, replace/insert one line.
std::expected<void, ConfigError>
write_default_toolchain(const GlobalConfig& cfg, std::string_view spec) {
    std::ifstream is(cfg.configFile);
    if (!is) return std::unexpected(ConfigError{
        std::format("cannot open '{}'", cfg.configFile.string())});
    std::stringstream ss; ss << is.rdbuf();
    std::string text = ss.str();

    std::string line = std::format("default = \"{}\"\n", spec);

    auto sectionPos = text.find("[toolchain]");
    if (sectionPos == std::string::npos) {
        // Append a [toolchain] block at end.
        if (!text.empty() && text.back() != '\n') text += '\n';
        text += std::format("\n[toolchain]\n{}", line);
    } else {
        // Locate existing `default = ...` within [toolchain]. If absent,
        // insert just after the section header.
        auto eol = text.find('\n', sectionPos);
        if (eol == std::string::npos) eol = text.size();
        auto bodyStart = (eol == text.size()) ? text.size() : eol + 1;
        auto nextSec = text.find("\n[", bodyStart);
        auto bodyEnd = (nextSec == std::string::npos) ? text.size() : nextSec;
        auto body = std::string_view(text).substr(bodyStart, bodyEnd - bodyStart);
        auto k = body.find("default");
        if (k != std::string_view::npos) {
            // replace that whole line
            auto kAbs = bodyStart + k;
            auto lineEnd = text.find('\n', kAbs);
            if (lineEnd == std::string::npos) lineEnd = text.size();
            text.replace(kAbs, lineEnd - kAbs + 1, line);
        } else {
            text.insert(bodyStart, line);
        }
    }

    std::ofstream os(cfg.configFile);
    if (!os) return std::unexpected(ConfigError{
        std::format("cannot write '{}'", cfg.configFile.string())});
    os << text;
    return {};
}

} // namespace mcpp::config
