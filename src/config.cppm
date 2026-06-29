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
#include <cstdlib>
#include <cstdio>

export module mcpp.config;

import std;
import mcpp.libs.toml;
import mcpp.pm.index_spec;
import mcpp.xlings;
import mcpp.platform;
import mcpp.log;
import mcpp.fallback.xlings_binary;
import mcpp.fallback.config_migration;
import mcpp.fallback.install_integrity;

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
#if defined(_WIN32)
    // On Windows, the copied xlings binary in the sandbox may not function
    // correctly for large package installs (missing runtime environment).
    // When MCPP_VENDORED_XLINGS is set, use the original xlings binary
    // directly — it has the full xlings runtime. The XLINGS_HOME env var
    // ensures packages are installed into the mcpp sandbox.
    if (auto* e = std::getenv("MCPP_VENDORED_XLINGS"); e && *e) {
        std::filesystem::path vendored{e};
        if (std::filesystem::exists(vendored))
            return { vendored, cfg.xlingsHome() };
    }
#endif
    return { cfg.xlingsBinary, cfg.xlingsHome() };
}

// Create an xlings::Env that targets the project-level .mcpp/ directory.
// Used when custom (non-builtin) indices are configured in mcpp.toml.
mcpp::xlings::Env make_project_xlings_env(const GlobalConfig& cfg,
                                           const std::filesystem::path& projectDir) {
    return { cfg.xlingsBinary, cfg.xlingsHome(), projectDir / ".mcpp" };
}

std::vector<std::filesystem::path>
project_xlings_data_roots(const std::filesystem::path& projectDir)
{
    auto dotMcpp = projectDir / ".mcpp";
    return {
        dotMcpp / "data",
        dotMcpp / ".xlings" / "data",
    };
}

std::filesystem::path
resolve_project_index_path(const std::filesystem::path& projectDir,
                           const mcpp::pm::IndexSpec& spec)
{
    auto source = spec.path;
    if (source.is_relative()) source = projectDir / source;
    return source.lexically_normal();
}

bool project_index_data_initialized(const std::filesystem::path& projectDir)
{
    std::error_code ec;
    for (auto const& data : project_xlings_data_roots(projectDir)) {
        auto reposDir = data / "xim-index-repos";
        if (std::filesystem::exists(reposDir / "xim-indexrepos.json", ec)) {
            return true;
        }
        if (std::filesystem::is_directory(reposDir, ec)) {
            for (auto const& entry : std::filesystem::directory_iterator(reposDir, ec)) {
                if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / "pkgs", ec)) {
                    return true;
                }
            }
        }
        if (std::filesystem::is_directory(data, ec)) {
            for (auto const& entry : std::filesystem::directory_iterator(data, ec)) {
                if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / "pkgs", ec)) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Check that the sandbox bootstrap completed successfully.
// Returns empty string if ok, or a description of what's missing.
std::string check_base_init(const GlobalConfig& cfg) {
    auto xlEnv = make_xlings_env(cfg);
    struct Check { std::string_view name; std::filesystem::path path; };

    auto patchelfBin = mcpp::xlings::paths::xim_tool(xlEnv, "patchelf",
        mcpp::xlings::pinned::kPatchelfVersion) / "bin" / "patchelf";
    auto ninjaRoot = mcpp::xlings::paths::xim_tool_root(xlEnv, "ninja");
    auto ninja_name = std::string("ninja") + std::string(mcpp::platform::exe_suffix);

    // Check xlings binary
    if (!std::filesystem::exists(cfg.xlingsBinary))
        return std::format("xlings binary missing: {}", cfg.xlingsBinary.string());

    // Check sandbox init marker
    auto marker = xlEnv.home / "subos" / "default" / ".xlings.json";
    if (!std::filesystem::exists(marker))
        return "sandbox not initialized (missing subos/default/.xlings.json)";

    // Check patchelf (Linux only)
#if !defined(__APPLE__) && !defined(_WIN32)
    if (!std::filesystem::exists(patchelfBin))
        return std::format("patchelf missing: {}", patchelfBin.string());
#endif

    // Check ninja
    bool ninjaOk = false;
    if (std::filesystem::exists(ninjaRoot)) {
        std::error_code ec;
        for (auto& v : std::filesystem::directory_iterator(ninjaRoot, ec)) {
            if (std::filesystem::exists(v.path() / ninja_name)) { ninjaOk = true; break; }
        }
    }
    if (!ninjaOk)
        return "ninja missing from sandbox";

    return {};  // all ok
}

// Delete the registry directory (sandbox) for a clean re-init.
// Preserves: bin/mcpp (self-contained mode), config.toml, log/.
void reset_registry(const GlobalConfig& cfg) {
    std::error_code ec;
    auto registry = cfg.xlingsHome();
    if (std::filesystem::exists(registry))
        std::filesystem::remove_all(registry, ec);

    // Also clear BMI and metadata caches (stale after registry reset).
    if (std::filesystem::exists(cfg.bmiCacheDir))
        std::filesystem::remove_all(cfg.bmiCacheDir, ec);
    if (std::filesystem::exists(cfg.metaCacheDir))
        std::filesystem::remove_all(cfg.metaCacheDir, ec);
}

// Ensure the project-level .mcpp/ directory exists and contains a
// .xlings.json seeded with custom non-builtin index entries
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

// `initial_mirror`: when seeding a *fresh* .xlings.json (file did not
// already exist), use this as the mirror value instead of the default.
// Empty means "use the default". Used by `mcpp self config --mirror X`
// so that the very first network roundtrip (during the immediately-
// following xlings sandbox bootstrap) goes through the user's chosen
// mirror, not the historical CN default. No effect when .xlings.json
// already exists — that case is handled by xlings::config_set_mirror.
std::expected<GlobalConfig, ConfigError> load_or_init(
    bool quiet = false,
    BootstrapProgressCallback onBootstrapProgress = {},
    std::string_view initial_mirror = {});

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

std::filesystem::path default_mcpp_home() {
    // Windows: %USERPROFILE%\.mcpp   POSIX: $HOME/.mcpp
    if constexpr (mcpp::platform::is_windows) {
        if (auto* e = std::getenv("USERPROFILE"); e && *e)
            return std::filesystem::path(e) / ".mcpp";
    }
    if (auto* e = std::getenv("HOME"); e && *e)
        return std::filesystem::path(e) / ".mcpp";
    return std::filesystem::current_path() / ".mcpp";
}

std::filesystem::path home_dir() {
    // 1. Explicit $MCPP_HOME takes priority (CI, advanced users).
    if (auto* e = std::getenv("MCPP_HOME"); e && *e)
        return std::filesystem::path(e);

    auto exe = mcpp::platform::fs::self_exe_path();
    if (exe.has_parent_path() && exe.parent_path().filename() == "bin") {
        auto candidate = exe.parent_path().parent_path();

        // Disqualify self-contained mode for two cases:
        //   a) Dev builds: .../target/<triple>/<fp>/bin/<exe>
        //   b) xlings packages: .../data/xpkgs/xim-x-mcpp/<ver>/bin/mcpp
        //      Creating a nested xlings sandbox inside the xpkgs directory
        //      breaks toolchain installation (nested XLINGS_HOME) and loses
        //      installed toolchains when the mcpp package version is upgraded.
        bool disqualified = false;
        for (auto p = candidate;
             p.has_parent_path() && p != p.root_path();
             p = p.parent_path()) {
            if (p.filename() == "target") { disqualified = true; break; }
            if (p.filename() == "xpkgs") {
                auto parent = p.parent_path().filename().string();
                if (parent == "data") { disqualified = true; break; }
            }
        }
        if (!disqualified)
            return candidate;
    }

    return default_mcpp_home();
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
                               const std::vector<IndexRepo>& repos,
                               std::string_view mirror_override = {})
{
    // Delegate to xlings module. Convert IndexRepo vec to pair span.
    std::vector<std::pair<std::string,std::string>> pairs;
    pairs.reserve(repos.size());
    for (auto& r : repos) pairs.emplace_back(r.name, r.url);
    // seed_xlings_json writes to env.home / ".xlings.json", so we
    // construct a temporary Env with home = path.parent_path().
    mcpp::xlings::Env env;
    env.home = path.parent_path();
    // No explicit --mirror: seed "auto" so xlings' own adaptive mirror module
    // (latency-probed, with per-download failover) picks the best reachable
    // host. The historic hardcoded "CN" FORCED the CN mirror and disabled that
    // mechanism, stranding overseas users and GitHub-hosted CI on a
    // slow/unreachable gitcode. An explicit --mirror always wins (else branch).
    if (mirror_override.empty())
        mcpp::xlings::seed_xlings_json(env, pairs);   // default = "auto"
    else
        mcpp::xlings::seed_xlings_json(env, pairs, mirror_override);
    return std::filesystem::exists(path);
}

// Migration helpers delegated to mcpp.fallback.config_migration.

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

// Xlings binary acquisition delegated to mcpp.fallback.xlings_binary.

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
    BootstrapProgressCallback onBootstrapProgress,
    std::string_view initial_mirror)
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
    cfg.xlingsBinary  = cfg.registryDir / "bin" /
        (std::string("xlings") + std::string(mcpp::platform::exe_suffix));
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

    // 2b. Initialize logger (early init with defaults; re-init after config load)
    {
        mcpp::log::Config logCfg;
        logCfg.logDir = cfg.logDir;
        mcpp::log::init(logCfg);
    }

    // 3. Seed config.toml if missing
    bool fresh_config = !std::filesystem::exists(cfg.configFile);
    if (fresh_config) write_default_config_toml(cfg.configFile);
    else mcpp::fallback::migrate_config_toml_index_names(cfg.configFile);

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

    // [log] section — re-initialize logger with config values
    {
        mcpp::log::Config logCfg;
        logCfg.logDir = cfg.logDir;
        auto levelStr = doc->get_string("log.level").value_or("off");
        if      (levelStr == "debug") logCfg.level = mcpp::log::Level::debug;
        else if (levelStr == "info")  logCfg.level = mcpp::log::Level::info;
        else if (levelStr == "warn")  logCfg.level = mcpp::log::Level::warn;
        else if (levelStr == "error") logCfg.level = mcpp::log::Level::error;
        logCfg.maxFileSize = static_cast<std::size_t>(
            doc->get_int("log.max_file_size").value_or(10 * 1024 * 1024));
        logCfg.maxFiles = static_cast<int>(
            doc->get_int("log.max_files").value_or(3));
        mcpp::log::init(logCfg);
    }

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
        write_default_xlings_json(xjson, cfg.indexRepos, initial_mirror);
    } else {
        mcpp::fallback::migrate_xlings_json_index_names(xjson);
    }

    // 6. Acquire xlings binary if needed
    if (cfg.xlingsBinaryMode == "bundled") {
        auto xbin = mcpp::fallback::acquire_xlings_binary(cfg.xlingsBinary, quiet);
        if (!xbin) return std::unexpected(ConfigError{xbin.error()});
    } else if (cfg.xlingsBinaryMode == "system") {
        auto sysPath = mcpp::platform::fs::which(
            std::string("xlings") + std::string(mcpp::platform::exe_suffix));
        if (!sysPath)
            return std::unexpected(ConfigError{"system xlings not found in PATH"});
        cfg.xlingsBinary = *sysPath;
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
    {
        auto bsEnv = make_xlings_env(cfg);
#if !defined(__APPLE__) && !defined(_WIN32)
        // patchelf is ELF-only; macOS uses Mach-O and Windows uses PE.
        ensure_sandbox_patchelf(cfg, quiet, onBootstrapProgress);
        // Only mark complete if the actual binary exists (not just the dir).
        {
            auto pBin = mcpp::xlings::paths::xim_tool(bsEnv, "patchelf",
                mcpp::xlings::pinned::kPatchelfVersion) / "bin" / "patchelf";
            if (std::filesystem::exists(pBin))
                mcpp::fallback::mark_install_complete(pBin.parent_path().parent_path());
        }
#endif
        ensure_sandbox_ninja(cfg, quiet, onBootstrapProgress);
        {
            auto nRoot = mcpp::xlings::paths::xim_tool_root(bsEnv, "ninja");
            auto ninja_name = std::string("ninja") + std::string(mcpp::platform::exe_suffix);
            std::error_code ec;
            if (std::filesystem::exists(nRoot)) {
                for (auto& v : std::filesystem::directory_iterator(nRoot, ec)) {
                    if (std::filesystem::exists(v.path() / ninja_name))
                        mcpp::fallback::mark_install_complete(v.path());
                }
            }
        }
    }

    // 8. Bootstrap check is NOT done here — it's deferred to commands that
    //    actually need bootstrap tools (build, run, toolchain install).
    //    Light commands (self env, toolchain list) should work even if
    //    bootstrap is incomplete. Commands call check_base_init() themselves.

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
    // Collect custom non-builtin indices that need xlings project-scope data.
    // Local path indices are also seeded so xlings can create its own
    // project-local repo link and install packages from that index.
    std::vector<std::pair<std::string,std::string>> customRepos;
    for (auto& [name, spec] : indices) {
        if (spec.is_builtin()) continue;
        if (spec.is_local()) {
            auto source = resolve_project_index_path(projectDir, spec);
            std::error_code ec;
            std::filesystem::remove(source / ".xlings-index-cache.json", ec);
            customRepos.emplace_back(name, source.generic_string());
            continue;
        }
        customRepos.emplace_back(name, spec.url);
    }

    auto officialIndex = cfg.xlingsHome() / "data" / "xim-pkgindex";
    std::error_code ec;
    if (!customRepos.empty() && std::filesystem::exists(officialIndex / "pkgs", ec)) {
        bool hasXim = false;
        for (auto const& [name, _] : customRepos) {
            if (name == "xim") {
                hasXim = true;
                break;
            }
        }
        if (!hasXim) {
            customRepos.emplace_back("xim", officialIndex.generic_string());
        }
    }

    if (customRepos.empty()) return false;  // nothing to do

    auto dotMcpp = projectDir / ".mcpp";
    std::filesystem::create_directories(dotMcpp, ec);

    // Seed .xlings.json with the custom index entries.
    mcpp::xlings::Env env;
    env.home = dotMcpp;
    mcpp::xlings::seed_xlings_json(env, customRepos);

    auto exposeLocalIndex = [&](const std::string& name,
                                const std::filesystem::path& source,
                                const std::filesystem::path& dataRoot)
    {
        std::error_code ec2;
        auto target = dataRoot / name;
        std::filesystem::create_directories(dataRoot, ec2);
        if (std::filesystem::exists(target / "pkgs", ec2)) {
            return;
        }
        ec2.clear();
        if (std::filesystem::exists(target, ec2) || std::filesystem::is_symlink(target, ec2)) {
            std::filesystem::remove_all(target, ec2);
        }
        ec2.clear();
        std::filesystem::create_directory_symlink(source, target, ec2);
        if (!ec2 && std::filesystem::exists(target / "pkgs", ec2)) {
            return;
        }
        ec2.clear();
        std::filesystem::copy(
            source,
            target,
            std::filesystem::copy_options::recursive
                | std::filesystem::copy_options::skip_existing,
            ec2);
    };

    for (auto& [name, spec] : indices) {
        if (spec.is_builtin() || !spec.is_local()) continue;
        auto source = resolve_project_index_path(projectDir, spec);
        if (!std::filesystem::exists(source / "pkgs", ec)) continue;
        exposeLocalIndex(name, source, dotMcpp / ".xlings" / "data");
        exposeLocalIndex(name, source, dotMcpp / "data");
    }

    // Project-scoped xlings installs custom-index packages in an additive
    // project data dir. Expose the global official xim index there too, so
    // package deps like `xim:python@latest` can resolve without falling back
    // to unrelated remote index updates or system tools.
    if (std::filesystem::exists(officialIndex / "pkgs", ec)) {
        auto projectData = dotMcpp / ".xlings" / "data";
        auto projectOfficial = projectData / "xim-pkgindex";
        std::filesystem::create_directories(projectData, ec);
        if (!std::filesystem::exists(projectOfficial, ec)) {
            std::filesystem::create_directory_symlink(officialIndex, projectOfficial, ec);
            if (ec) {
                ec.clear();
                std::filesystem::copy(
                    officialIndex,
                    projectOfficial,
                    std::filesystem::copy_options::recursive
                        | std::filesystem::copy_options::skip_existing,
                    ec);
            }
        }
    }
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
