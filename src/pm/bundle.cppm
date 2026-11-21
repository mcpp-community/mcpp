// mcpp.pm.bundle — a project's inputs as a directory, and the directory taken
// into a home (`mcpp fetch --to`, `mcpp fetch --from`; design 2026-11-21,
// offline and toolchain defaults §3.2).
//
// THE PROVIDER WRITES THE DIRECTORY AND MCPP ADDS TO IT. `xlings download
// --to <dir> --with-index` writes the archives of the packages' closures, a
// snapshot of each index that resolved them and `bundle.toml`; mcpp adds a
// `git bundle` of each git clone the build used and its own tables, `[mcpp]`
// and `[[git]]`, which the provider keeps when it rewrites the manifest. The
// directory's layout is not an interface: a reader takes paths from the
// manifest, and a directory one release writes, that release and every later
// one read.
export module mcpp.pm.bundle;

import std;
import mcpp.config;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.effects;
import mcpp.run;
import mcpp.home;
import mcpp.home.files;
import mcpp.libs.toml;
import mcpp.pm.lock_io;
import mcpp.toolchain.platform;
import mcpp.build.refusal;
import mcpp.diag;
import mcpp.ui;
import mcpp.version;

export namespace mcpp::pm::bundle {

// The format of mcpp's own tables; a reader refuses a higher one.
inline constexpr std::int64_t kFormat = 1;

// This machine as a bundle names a host: `<os>-<arch>` in the payloads'
// vocabulary (`linux-x86_64`).
std::string this_host();

struct Written {
    std::size_t packages = 0;
    std::size_t clones = 0;
    std::uintmax_t bytes = 0;
};

// `mcpp fetch --to <dir>`, after the build's resolution installed what it
// needs: every payload this run used (`mcpp::effects::packages_used`), with
// the closure the provider adds, and every git clone. `targets` are the
// targets resolved, recorded for `--from` to compare with its own.
std::expected<Written, std::string>
write(const std::filesystem::path& dir, const mcpp::config::GlobalConfig& cfg,
      const std::filesystem::path& projectRoot, std::span<const std::string> targets);

// A bundle as `mcpp fetch --from` reads it: its manifest, judged against
// this machine and the project's lock.
struct GitEntry {
    std::string url, commit, clone, file;
};
// A named struct, not a `std::pair`: an exported pair of strings truncates the
// BMI under GCC 16 ("failed to read compiled module cluster ...: Bad file
// data" in an importer), measured while writing this module.
struct IndexEntry {
    std::string name, path;
};
struct Bundle {
    std::filesystem::path dir;
    std::string host;                                            // `[mcpp] host`
    std::vector<std::string> targets, packages;                  // `[mcpp]`
    std::vector<IndexEntry> indices;                             // `[[index]]`
    std::vector<GitEntry> git;                                   // `[[git]]`
};

// The bundle at `dir`, refused when mcpp did not write it, a newer mcpp did,
// or it lacks a version mcpp.lock records (bundle-older-than-lock). A host or
// a target of `targets` it was not written for is said, and it is read.
std::expected<Bundle, std::string>
open(const std::filesystem::path& dir, const std::filesystem::path& projectRoot,
     std::span<const std::string> targets);

// Its index snapshots made the home's indices where the home has none of its
// own (Provider::adopt_index_snapshot): before the home's bootstrap, whose
// tools resolve with them. The names of those placed.
std::vector<std::string> adopt_indices(const Bundle& b, const mcpp::config::GlobalConfig& cfg,
                                       const std::filesystem::path& projectRoot);

struct Imported {
    std::size_t packages = 0;
    std::size_t clones = 0;
};

// Its packages installed resolving with its snapshots, its clones imported
// where a build looks for them. Afterwards the directory may be removed.
std::expected<Imported, std::string>
install(const Bundle& b, const mcpp::config::GlobalConfig& cfg, const std::filesystem::path& projectRoot);

} // namespace mcpp::pm::bundle

namespace mcpp::pm::bundle {

namespace {

namespace fs = std::filesystem;
namespace toml = mcpp::libs::toml;

// The provider's view for a project: its own when the project names indices
// of its own (the provider has written its configuration there), else the
// home's. The provider names its configuration file; the layout is its own.
mcpp::provider::Location location_for(const mcpp::config::GlobalConfig& cfg, const fs::path& projectRoot) {
    if (!projectRoot.empty()) {
        auto project = mcpp::config::make_project_xlings_env(cfg, projectRoot);
        std::error_code ec;
        if (fs::is_regular_file(mcpp::provider::open(project)->config_file(), ec)) return project;
    }
    return mcpp::config::make_xlings_env(cfg);
}

std::string quoted_list(std::span<const std::string> items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i ? ", " : "") + toml::escape_string(items[i]);
    return out + "]";
}

// The manifest without mcpp's own tables: each `[mcpp]`, `[mcpp.*]` and
// `[[git]]` header to the next header is left out; the provider's tables and
// any other program's are kept as written.
std::string without_own_tables(std::string_view text) {
    std::string out;
    bool skipping = false;
    for (auto part : std::views::split(text, '\n')) {
        std::string_view line(part.begin(), part.end());
        auto t = line;
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
        if (t.starts_with('[')) {
            skipping = t.starts_with("[mcpp]") || t.starts_with("[mcpp.") || t.starts_with("[[git]]");
        }
        if (!skipping) { out += line; out += '\n'; }
    }
    while (out.size() >= 2 && out.ends_with("\n\n")) out.pop_back();
    return out;
}

std::uintmax_t size_of(const fs::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    return total;
}

std::string git_out(std::vector<std::string> argv) {
    auto r = mcpp::run::capture_stdout(argv);
    if (r.exit_code != 0) return {};
    auto s = r.output;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

struct Manifest : Bundle {
    std::int64_t format = 0;          // `[mcpp] format`; 0: no mcpp tables
};

std::expected<Manifest, std::string> read_manifest(const fs::path& dir) {
    const auto file = dir / "bundle.toml";
    std::error_code ec;
    if (!fs::is_regular_file(file, ec))
        return std::unexpected(std::format("{} holds no bundle.toml; it is not a directory `mcpp fetch --to` wrote",
                                           mcpp::home::spelling(dir)));
    auto doc = toml::parse_file(file);
    if (!doc) return std::unexpected(std::format("{}: {}", mcpp::home::spelling(file), doc.error().message));
    Manifest m;
    m.dir = dir;
    m.format = doc->get_int("mcpp.format").value_or(0);
    m.host = doc->get_string("mcpp.host").value_or("");
    m.targets = doc->get_string_array("mcpp.targets").value_or(std::vector<std::string>{});
    m.packages = doc->get_string_array("mcpp.packages").value_or(std::vector<std::string>{});
    auto str = [](const toml::Table& t, std::string_view key) {
        auto it = t.find(std::string(key));
        return it != t.end() && it->second.is_string() ? it->second.as_string() : std::string{};
    };
    auto tables = [&](std::string_view key) {
        std::vector<const toml::Table*> out;
        if (auto* v = doc->get(key); v && v->is_array())
            for (auto const& e : v->as_array())
                if (e.is_table()) out.push_back(&e.as_table());
        return out;
    };
    for (auto* t : tables("index")) m.indices.push_back({str(*t, "name"), str(*t, "path")});
    for (auto* t : tables("git"))
        m.git.push_back({str(*t, "url"), str(*t, "commit"), str(*t, "clone"), str(*t, "file")});
    return m;
}

// A path of the manifest, relative to the bundle and inside it.
std::optional<fs::path> inside(const fs::path& dir, std::string_view rel) {
    if (rel.empty()) return std::nullopt;
    const auto p = (dir / fs::path(std::string(rel))).lexically_normal();
    const auto r = p.lexically_relative(dir.lexically_normal());
    if (r.empty() || *r.begin() == "..") return std::nullopt;
    return p;
}

// THE LOCK AGAINST THE BUNDLE. A version mcpp.lock records for a package of
// an index, or for a toolchain request, that the bundle does not carry.
std::vector<std::string> missing_from(const Bundle& m, const fs::path& projectRoot) {
    std::vector<std::string> out;
    std::error_code ec;
    const auto lockPath = projectRoot / "mcpp.lock";
    if (projectRoot.empty() || !fs::is_regular_file(lockPath, ec)) return out;
    auto lock = mcpp::pm::load(lockPath);
    if (!lock) return out;
    auto carried = [&](std::string_view name, std::string_view version) {
        return std::ranges::any_of(m.packages, [&](const std::string& ref) {
            const auto at = ref.rfind('@');
            if (at == std::string::npos || std::string_view(ref).substr(at + 1) != version) return false;
            std::string_view n = std::string_view(ref).substr(0, at);
            if (auto colon = n.find(':'); colon != std::string_view::npos) n.remove_prefix(colon + 1);
            return n == name || name.ends_with(std::string(".") + std::string(n));
        });
    };
    for (auto const& p : lock->packages)
        if (p.source.starts_with("index+") && !carried(p.name, p.version))
            out.push_back(std::format("{} {}", p.name, p.version));
    for (auto const& t : lock->toolchains) {
        std::string_view req = t.request;
        if (auto colon = req.find(':'); colon != std::string_view::npos) req.remove_prefix(colon + 1);
        const auto name = req.substr(0, req.find('@'));
        if (!carried(name, t.version)) out.push_back(std::format("toolchain {} {}", t.request, t.version));
    }
    return out;
}

} // namespace

std::string this_host() {
    return std::format("{}-{}", mcpp::toolchain::platform::xpkg_platform, mcpp::toolchain::platform::host_arch);
}

std::expected<Written, std::string>
write(const fs::path& dirIn, const mcpp::config::GlobalConfig& cfg,
      const fs::path& projectRoot, std::span<const std::string> targets) {
    std::error_code ec;
    const auto dir = fs::absolute(dirIn, ec).lexically_normal();
    fs::create_directories(dir, ec);
    if (ec) return std::unexpected(std::format("cannot create {}: {}", mcpp::home::spelling(dir), ec.message()));

    // The payloads, and the build tool the build ran with unless the machine's
    // is taken: it is installed beside the payloads, not resolved by them.
    auto refs = mcpp::effects::packages_used();
    if (!mcpp::effects::host_tool_allowed("ninja"))
        refs.push_back(std::format("xim:ninja@{}", mcpp::provider::kNinjaVersion));
    std::ranges::sort(refs);
    refs.erase(std::ranges::unique(refs).begin(), refs.end());
    const auto provider = mcpp::provider::open(location_for(cfg, projectRoot));
    if (int rc = provider->download_to(refs, dir); rc != 0)
        return std::unexpected(std::format("writing the archives to {} failed (provider exit {})", mcpp::home::spelling(dir), rc));

    // Each clone at the commit the build used, as one file.
    std::vector<GitEntry> git;
    for (auto const& g : mcpp::effects::git_used()) {
        const auto commit = git_out({"git", "-C", mcpp::home::spelling(g.clone), "rev-parse", "HEAD"});
        if (commit.empty())
            return std::unexpected(std::format("{} is not a git clone mcpp can read", mcpp::home::spelling(g.clone)));
        const auto name = mcpp::home::spelling(g.clone.filename());
        const auto rel = std::format("git/{}.bundle", name);
        fs::create_directories(dir / "git", ec);
        auto r = mcpp::run::execute({"git", "-C", mcpp::home::spelling(g.clone), "bundle", "create", "--quiet",
                                     mcpp::home::spelling(dir / rel), "HEAD"});
        if (r.exit_code != 0)
            return std::unexpected(std::format("`git bundle create` of {} failed:\n{}", g.url, r.output));
        git.push_back({g.url, commit, name, rel});
    }

    // mcpp's tables after the provider's, replaced whole.
    const auto file = dir / "bundle.toml";
    std::string text;
    if (std::ifstream in{file, std::ios::binary}; in) text.assign(std::istreambuf_iterator<char>(in), {});
    std::string own = std::format(
        "\n[mcpp]\nformat = {}\ncreated_by = {}\nhost = {}\ntargets = {}\npackages = {}\n",
        kFormat, toml::escape_string(std::format("mcpp {}", mcpp::MCPP_VERSION)),
        toml::escape_string(this_host()), quoted_list(std::vector<std::string>(targets.begin(), targets.end())),
        quoted_list(refs));
    for (auto const& g : git)
        own += std::format("\n[[git]]\nurl = {}\ncommit = {}\nclone = {}\nfile = {}\n",
                           toml::escape_string(g.url), toml::escape_string(g.commit),
                           toml::escape_string(g.clone), toml::escape_string(g.file));
    if (!mcpp::home::write_replacing(file, without_own_tables(text) + own))
        return std::unexpected(std::format("cannot write {}", mcpp::home::spelling(file)));
    return Written{refs.size(), git.size(), size_of(dir)};
}

std::expected<Bundle, std::string>
open(const fs::path& dirIn, const fs::path& projectRoot, std::span<const std::string> targets) {
    std::error_code ec;
    const auto dir = fs::absolute(dirIn, ec).lexically_normal();
    auto m = read_manifest(dir);
    if (!m) return std::unexpected(m.error());
    if (m->format == 0)
        return std::unexpected(std::format("{}/bundle.toml has no [mcpp] table; `mcpp fetch --to` writes one",
                                           mcpp::home::spelling(dir)));
    if (m->format > kFormat)
        return std::unexpected(std::format("{} was written by a newer mcpp (bundle format {}, this one reads {})\n"
                                           "  help: `mcpp self update`", mcpp::home::spelling(dir), m->format, kFormat));
    for (auto const& ix : m->indices)
        if (!inside(dir, ix.path))
            return std::unexpected(std::format("{}/bundle.toml: index `{}` names {}, outside the bundle",
                                               mcpp::home::spelling(dir), ix.name, ix.path));
    for (auto const& g : m->git)
        if (!inside(dir, g.file) || g.clone.empty() || g.clone.find_first_of("/\\") != std::string::npos)
            return std::unexpected(std::format("{}/bundle.toml: the [[git]] entry for {} is malformed",
                                               mcpp::home::spelling(dir), g.url));

    // What the bundle was not written for is said, and the rest goes on: a
    // payload that only another host or target needs is not one this
    // machine misses.
    if (m->host != this_host())
        mcpp::diag::notice(mcpp::diag::Severity::Warning, "bundle-lacks-target", std::format("host\x1f{}", m->host),
            std::format("{} was written on {}, and this machine is {}: its archives may not hold this host's payloads",
                        mcpp::home::spelling(dir), m->host, this_host()),
            "write the bundle on a machine of this host");
    for (auto const& t : targets)
        if (!t.empty() && std::ranges::find(m->targets, t) == m->targets.end())
            mcpp::diag::notice(mcpp::diag::Severity::Warning, "bundle-lacks-target", std::format("target\x1f{}", t),
                std::format("{} was written for {}, not for {}", mcpp::home::spelling(dir),
                            m->targets.empty() ? std::string("the host target") : quoted_list(m->targets), t),
                std::format("write it with `mcpp fetch --to <dir> --target {}`", t));

    if (auto missing = missing_from(*m, projectRoot); !missing.empty()) {
        mcpp::build::refusal::record(mcpp::build::refusal::Code::BundleOlderThanLock);
        std::string list;
        for (auto const& s : missing) list += "\n  " + s;
        return std::unexpected(std::format(
            "{} does not carry what mcpp.lock records:{}\n"
            "  help: write the bundle again from this lock (`mcpp fetch --to <dir>` on a machine with a network)",
            mcpp::home::spelling(dir), list));
    }
    return static_cast<Bundle>(*m);
}

std::vector<std::string> adopt_indices(const Bundle& b, const mcpp::config::GlobalConfig& cfg,
                                       const fs::path& projectRoot) {
    std::vector<std::string> placed;
    const auto provider = mcpp::provider::open(location_for(cfg, projectRoot));
    for (auto const& ix : b.indices)
        if (auto snapshot = inside(b.dir, ix.path); snapshot && provider->adopt_index_snapshot(ix.name, *snapshot) == "placed")
            placed.push_back(ix.name);
    return placed;
}

std::expected<Imported, std::string>
install(const Bundle& b, const mcpp::config::GlobalConfig& cfg, const fs::path& projectRoot) {
    std::error_code ec;
    Imported out;
    const auto provider = mcpp::provider::open(location_for(cfg, projectRoot));
    if (!b.packages.empty()) {
        if (int rc = provider->install_from(b.dir, b.packages); rc != 0) {
            if (mcpp::effects::offline_mode())
                mcpp::build::refusal::record(mcpp::build::refusal::Code::ArtifactUnavailableOffline);
            return std::unexpected(std::format("installing from {} failed (provider exit {}); "
                                               "the provider's report is above", mcpp::home::spelling(b.dir), rc));
        }
    }
    out.packages = b.packages.size();

    // Each clone where a build looks for it, at its commit, its remote the
    // dependency's own URL.
    for (auto const& g : b.git) {
        const auto file = *inside(b.dir, g.file);
        const auto dest = mcpp::home::root() / "git" / g.clone;
        if (fs::exists(dest / ".git", ec)) continue;
        fs::remove_all(dest, ec);
        fs::create_directories(dest.parent_path(), ec);
        auto cl = mcpp::run::execute({"git", "clone", "--quiet", mcpp::home::spelling(file), mcpp::home::spelling(dest)});
        if (cl.exit_code == 0) cl = mcpp::run::execute({"git", "-C", mcpp::home::spelling(dest), "checkout", "--quiet", g.commit});
        if (cl.exit_code == 0) cl = mcpp::run::execute({"git", "-C", mcpp::home::spelling(dest), "remote", "set-url", "origin", g.url});
        if (cl.exit_code != 0) {
            fs::remove_all(dest, ec);
            return std::unexpected(std::format("importing the clone of {} failed:\n{}", g.url, cl.output));
        }
        ++out.clones;
    }
    return out;
}

} // namespace mcpp::pm::bundle
