// mcpp.config — global config + paths + xlings binary acquisition.
//
// Layout (per docs/14-data-layout.md):
//   $MCPP_HOME/                 default ~/.mcpp/
//     bin/mcpp                  mcpp binary (self-contained mode)
//     registry/                 XLINGS_HOME for mcpp's xlings
//       bin/xlings              vendored xlings binary (= <XLINGS_HOME>/bin/xlings)
//       .xlings.json            seeded with index_repos = [mcpplibs]
//     bmi/<fp>/                 BMI cache (existing)
//     cache/                    metadata caches
//     config.toml               this module's input
//
// Initialization is silent and idempotent: every Config::load() ensures
// the directory tree exists and seeds .xlings.json if missing.

module;
#include <cstdio>
#include <cstdlib>
#if defined(__APPLE__)
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#endif

export module mcpp.config;

import std;
import mcpp.libs.toml;
import mcpp.pm.index_spec;
import mcpp.xlings;

export namespace mcpp::config {

inline constexpr std::string_view kXlingsPinnedVersion = mcpp::xlings::pinned::kXlingsVersion;

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
    std::string                     defaultIndex;        // "mcpplibs"
    std::vector<IndexRepo>          indexRepos;

    // From config.toml [indices] — custom index repositories (new schema).
    // Merged: project mcpp.toml > global config.toml > built-in default.
    std::map<std::string, mcpp::pm::IndexSpec> indices;

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

// Create an xlings::Env from the resolved GlobalConfig.
mcpp::xlings::Env make_xlings_env(const GlobalConfig& cfg) {
    return { cfg.xlingsBinary, cfg.xlingsHome() };
}

// Create an xlings::Env that targets the project-level .mcpp/ directory.
// Used when custom (non-builtin) indices are configured in mcpp.toml.
mcpp::xlings::Env make_project_xlings_env(const GlobalConfig& cfg,
                                           const std::filesystem::path& projectDir) {
    return { cfg.xlingsBinary, cfg.xlingsHome(), projectDir / ".mcpp" };
}

// Ensure the project-level .mcpp/ directory exists and contains a
// .xlings.json seeded with the custom (non-builtin, non-local) index
// entries. Returns true if a .mcpp/ directory was created/updated.
bool ensure_project_index_dir(
    const GlobalConfig& cfg,
    const std::filesystem::path& projectDir,
    const std::map<std::string, mcpp::pm::IndexSpec>& indices);

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
using BootstrapFile             = mcpp::xlings::BootstrapFile;
using BootstrapProgress         = mcpp::xlings::BootstrapProgress;
using BootstrapProgressCallback = mcpp::xlings::BootstrapProgressCallback;

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
#if defined(__APPLE__)
    char _exe_buf[4096];
    uint32_t _exe_size = sizeof(_exe_buf);
    std::filesystem::path exe;
    if (_NSGetExecutablePath(_exe_buf, &_exe_size) == 0)
        exe = std::filesystem::canonical(_exe_buf, ec);
#else
    auto exe = std::filesystem::canonical("/proc/self/exe", ec);
#endif
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
    return mcpp::xlings::run_capture(cmd);
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
default = "mcpplibs"

[index.repos."mcpplibs"]
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
    // Delegate to xlings module. Convert IndexRepo vec to pair span.
    std::vector<std::pair<std::string,std::string>> pairs;
    pairs.reserve(repos.size());
    for (auto& r : repos) pairs.emplace_back(r.name, r.url);
    // seed_xlings_json writes to env.home / ".xlings.json", so we
    // construct a temporary Env with home = path.parent_path().
    mcpp::xlings::Env env;
    env.home = path.parent_path();
    mcpp::xlings::seed_xlings_json(env, pairs);
    return std::filesystem::exists(path);
}

bool replace_all(std::string& text, std::string_view from, std::string_view to) {
    bool changed = false;
    for (std::size_t pos = 0;
         (pos = text.find(from, pos)) != std::string::npos;) {
        text.replace(pos, from.size(), to);
        pos += to.size();
        changed = true;
    }
    return changed;
}

bool write_text_if_changed(const std::filesystem::path& path,
                           const std::string& original,
                           const std::string& updated) {
    if (updated == original) return false;
    write_file(path, updated);
    return true;
}

bool migrate_legacy_config_toml_index_names(const std::filesystem::path& path) {
    std::ifstream is(path);
    if (!is) return false;
    std::stringstream ss;
    ss << is.rdbuf();
    auto original = ss.str();
    auto updated = original;

    replace_all(updated, "default = \"mcpp-index\"", "default = \"mcpplibs\"");
    replace_all(updated, "[index.repos.\"mcpp-index\"]", "[index.repos.\"mcpplibs\"]");

    return write_text_if_changed(path, original, updated);
}

bool migrate_legacy_xlings_json_index_names(const std::filesystem::path& path) {
    std::ifstream is(path);
    if (!is) return false;
    std::stringstream ss;
    ss << is.rdbuf();
    auto original = ss.str();
    auto updated = original;

    // Older mcpp sandboxes seeded the package index under the repository
    // name. Keep the URL and all xlings state intact; only rename the index
    // key so xlings config/list output matches mcpp's default namespace.
    replace_all(updated, "\"name\": \"mcpp-index\"", "\"name\": \"mcpplibs\"");
    replace_all(updated, "\"name\":\"mcpp-index\"", "\"name\":\"mcpplibs\"");

    return write_text_if_changed(path, original, updated);
}

void canonicalize_legacy_index_names(GlobalConfig& cfg) {
    if (cfg.defaultIndex == "mcpp-index")
        cfg.defaultIndex = "mcpplibs";

    std::vector<IndexRepo> normalized;
    for (auto r : cfg.indexRepos) {
        if (r.name == "mcpp-index"
            && r.url == "https://github.com/mcpp-community/mcpp-index.git") {
            r.name = "mcpplibs";
        }
        bool duplicate = std::any_of(normalized.begin(), normalized.end(),
            [&](const IndexRepo& existing) {
                return existing.name == r.name && existing.url == r.url;
            });
        if (!duplicate) normalized.push_back(std::move(r));
    }
    cfg.indexRepos = std::move(normalized);
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
    mcpp::xlings::ensure_init(make_xlings_env(cfg), quiet);
}

// With the 0.0.4 layout change (xlings binary at <MCPP_HOME>/registry/bin/
// = <XLINGS_HOME>/bin/), the bundled xlings IS already at the path xlings's
// shim-creation guard checks (`paths.homeDir / "bin" / "xlings"`).
// No mirroring / hardlinking needed — this function is now a no-op.
void ensure_sandbox_xlings_binary(const GlobalConfig& /*cfg*/, bool /*quiet*/) noexcept {
    // Intentional no-op: xlingsBinary == xlingsHome()/bin/xlings.
}

// Bootstrap install: delegated to mcpp.xlings module.

void ensure_sandbox_ninja(const GlobalConfig& cfg, bool quiet,
                          const BootstrapProgressCallback& cb) noexcept
{
    mcpp::xlings::ensure_ninja(make_xlings_env(cfg), quiet, cb);
}

void ensure_sandbox_patchelf(const GlobalConfig& cfg, bool quiet,
                              const BootstrapProgressCallback& cb) noexcept
{
    mcpp::xlings::ensure_patchelf(make_xlings_env(cfg), quiet, cb);
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
    else migrate_legacy_config_toml_index_names(cfg.configFile);

    // 4. Load config.toml
    auto doc = t::parse_file(cfg.configFile);
    if (!doc) {
        return std::unexpected(ConfigError{
            std::format("invalid config.toml: {}", doc.error().message)});
    }

    cfg.xlingsBinaryMode = doc->get_string("xlings.binary").value_or("bundled");
    if (auto h = doc->get_string("xlings.home"); h && !h->empty())
        cfg.xlingsHomeOverride = *h;
    cfg.defaultIndex   = doc->get_string("index.default").value_or("mcpplibs");
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
    // [indices] — new-schema custom index repositories.
    // Accepts the same short/long/path forms as mcpp.toml [indices].
    if (auto* indices_t = doc->get_table("indices")) {
        for (auto& [k, v] : *indices_t) {
            mcpp::pm::IndexSpec spec;
            spec.name = k;
            if (v.is_string()) {
                spec.url = v.as_string();
            } else if (v.is_table()) {
                auto& sub = v.as_table();
                if (auto it = sub.find("url");    it != sub.end() && it->second.is_string()) spec.url    = it->second.as_string();
                if (auto it = sub.find("rev");    it != sub.end() && it->second.is_string()) spec.rev    = it->second.as_string();
                if (auto it = sub.find("tag");    it != sub.end() && it->second.is_string()) spec.tag    = it->second.as_string();
                if (auto it = sub.find("branch"); it != sub.end() && it->second.is_string()) spec.branch = it->second.as_string();
                if (auto it = sub.find("path");   it != sub.end() && it->second.is_string()) spec.path   = it->second.as_string();
            }
            if (!spec.url.empty() || !spec.path.empty())
                cfg.indices[k] = std::move(spec);
        }
    }

    // Defaults: only mcpplibs. xlings auto-adds its own standard
    // defaults (xim / awesome / scode / d2x) because globalIndexRepos_
    // is non-empty (per xlings/src/core/config.cppm). Explicitly listing
    // them ourselves can cause cross-index name conflicts during
    // dependency resolution (e.g. linux-headers existing in both
    // scode and xim). See docs/21 §VII.
    auto add_default = [&](std::string_view name, std::string_view url) {
        for (auto& r : cfg.indexRepos) if (r.name == name) return;
        cfg.indexRepos.push_back({ std::string(name), std::string(url) });
    };
    add_default("mcpplibs", "https://github.com/mcpp-community/mcpp-index.git");
    canonicalize_legacy_index_names(cfg);

    // 5. Seed registry/.xlings.json if missing; migrate legacy cached
    // sandboxes in place so CI/user caches move from mcpp-index to mcpplibs
    // without losing xlings' active subos or version bindings.
    auto xjson = cfg.xlingsHome() / ".xlings.json";
    if (!std::filesystem::exists(xjson)) {
        write_default_xlings_json(xjson, cfg.indexRepos);
    } else {
        migrate_legacy_xlings_json_index_names(xjson);
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
#if !defined(__APPLE__)
    // patchelf is ELF-only; macOS uses Mach-O and does not need it.
    ensure_sandbox_patchelf(cfg, quiet, onBootstrapProgress);
#endif
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

bool ensure_project_index_dir(
    const GlobalConfig& cfg,
    const std::filesystem::path& projectDir,
    const std::map<std::string, mcpp::pm::IndexSpec>& indices)
{
    // Collect custom (non-builtin, non-local) indices that need xlings cloning.
    std::vector<std::pair<std::string,std::string>> customRepos;
    for (auto& [name, spec] : indices) {
        if (spec.is_builtin()) continue;
        if (spec.is_local())   continue;   // local path, mcpp reads directly
        customRepos.emplace_back(name, spec.url);
    }

    if (customRepos.empty()) return false;  // nothing to do

    auto dotMcpp = projectDir / ".mcpp";
    std::error_code ec;
    std::filesystem::create_directories(dotMcpp, ec);

    // Seed .xlings.json with the custom index entries.
    mcpp::xlings::Env env;
    env.home = dotMcpp;
    mcpp::xlings::seed_xlings_json(env, customRepos);
    return true;
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
