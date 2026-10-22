// mcpp.home — the single resolver for MCPP_HOME and its well-known subdirs.
//
// Every path under the mcpp home must be derived from here. Before #311 this
// logic existed in three places (config.cppm, toolchain/stdmod.cppm and an
// inline lambda in build/prepare.cppm); the copies drifted, and the oldest one
// (stdmod, unchanged since v0.0.1) never learned about Windows' USERPROFILE or
// about self-contained installs. The observable damage: on Windows PowerShell
// (no $HOME) the std BMI cache landed in the *current working directory* as
// `.mcpp-bmi/`, while dep BMIs went to %USERPROFILE%\.mcpp\bmi — two roots for
// one cache, and a cwd-dependent one at that.
//
// A leaf over platform and the TOML reader: configuration, toolchains and
// builds share its single path selection without depending on CLI or provider.

module;
#include <cstdlib>    // getenv

export module mcpp.home;

import std;
import mcpp.platform;
import mcpp.libs.toml;

export namespace mcpp::home {

// The system-level configuration (design 2026-10-16 §8.1, §9): what an
// administrator states for every user of the machine -- upstreams, a default
// toolchain, the system home. $MCPP_SYSTEM_CONFIG when set; else
// /etc/mcpp/config.toml (Windows: %ProgramData%\mcpp\config.toml).
std::filesystem::path system_config_file();

// The system home (§9.1): `[system] home` of the system configuration; else
// /var/lib/mcpp (macOS: /Library/Application Support/mcpp; Windows:
// %ProgramData%\mcpp). Written only by `mcpp --system`; read by every user.
std::filesystem::path system_root();

// The marker a system home carries (`.mcpp-home`, mode = "system"), and
// whether `home` has it.
inline constexpr std::string_view kHomeMarker = ".mcpp-home";
bool is_system_home(const std::filesystem::path& home);


// $MCPP_HOME. See root() below for the resolution order.
std::filesystem::path root();

// The original directory owns the one-hop locator; every data consumer uses
// effective. No environment variable is rewritten to implement a selection.
struct Selection {
    std::filesystem::path original;
    std::filesystem::path effective;
    std::filesystem::path previous;
    std::string source;
    bool redirected = false;
};
struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
std::string spelling(const std::filesystem::path& path);
std::filesystem::path from_utf8(std::string_view value);
std::filesystem::path original_root();
std::string original_source();
std::expected<Selection, std::string> inspect(const std::filesystem::path& original);
Selection selection();
bool initialized(const std::filesystem::path& path);
std::expected<Selection, std::string> choose(const std::filesystem::path& target, bool force);
std::expected<void, std::string> save(const Selection& selected);
void use(const Selection& selected);

// The cross-project build cache root ($MCPP_HOME/build-cache/v1). Holds both
// the std module cache (toolchain/stdmod.cppm, under std/) and the per-package
// dependency cache (bmi_cache.cppm, under pkg/) — they MUST agree.
//
// NOT `$MCPP_HOME/cache`: that name is already taken by GlobalConfig's index
// metadata cache, which config.cppm's reset path deletes wholesale. Nesting
// compiled artifacts inside a directory something else clears is a trap that
// would surface as "the build cache empties itself sometimes".
//
// The `v1` segment is the layout version. It exists so replacing the layout
// never has to migrate or delete anything: a new layout is a new segment, and
// the old tree becomes inert until `mcpp cache clean --legacy` collects it.
std::filesystem::path cache_root();

// The pre-v1 cache root ($MCPP_HOME/bmi), keyed by whole-project fingerprint.
// Nothing reads or writes it any more; `mcpp doctor` reports it and
// `mcpp cache clean --legacy` removes it. Kept resolvable rather than
// hardcoded at the call sites so "where is the old cache" has one answer too.
std::filesystem::path legacy_bmi_root();

} // namespace mcpp::home

namespace mcpp::home {

namespace {

std::filesystem::path default_home() {
    // Windows: %USERPROFILE%\.mcpp   POSIX: $HOME/.mcpp
    if constexpr (mcpp::platform::is_windows) {
        if (auto* e = std::getenv("USERPROFILE"); e && *e)
            return std::filesystem::path(e) / ".mcpp";
    }
    if (auto* e = std::getenv("HOME"); e && *e)
        return std::filesystem::path(e) / ".mcpp";
    return std::filesystem::current_path() / ".mcpp";
}

} // namespace

// Resolve MCPP_HOME, in priority order:
//   1. $MCPP_HOME env var (explicit override — used by CI / dev / multi-instance)
//   2. <binary-dir>/.. — self-contained mode, when mcpp lives at
//      <root>/bin/mcpp. Release tarballs and `xlings install mcpp`
//      use this layout; the unpacked tree IS the home. Dev builds
//      live under target/<triple>/<fp>/bin/mcpp, which is the same
//      "in a bin/ dir" shape — so we additionally exclude any path
//      with a "target" ancestor as mcpp's own dev convention.
//   3. fallback to $HOME/.mcpp (%USERPROFILE%\.mcpp on Windows).
namespace {
std::filesystem::path portable_root() {
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
            if (p.filename() == "xpkgs") {  // provider-boundary-ok: mcpp.home sits below the port (the adapter imports it)
                auto parent = p.parent_path().filename().string();
                if (parent == "data") { disqualified = true; break; }
            }
        }
        if (!disqualified)
            return candidate;
    }

    return {};
}
} // namespace

std::filesystem::path original_root() {
    if (auto* e = std::getenv("MCPP_HOME"); e && *e) return std::filesystem::path(e);
    const auto portable = portable_root();
    return portable.empty() ? default_home() : portable;
}

std::string original_source() {
    if (auto* e = std::getenv("MCPP_HOME"); e && *e) return "environment";
    return portable_root().empty() ? "user" : "portable";
}

std::string spelling(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::filesystem::path from_utf8(std::string_view value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

namespace {
std::optional<Selection> selected_home;

std::expected<std::filesystem::path, std::string> absolute_home(const std::filesystem::path& path) {
    std::error_code ec;
    auto result = std::filesystem::weakly_canonical(std::filesystem::absolute(path, ec), ec);
    if (ec) return std::unexpected(std::format("cannot resolve home '{}': {}", spelling(path), ec.message()));
    return result;
}

bool contains_path(const std::filesystem::path& parent, const std::filesystem::path& child) {
    // Case folding matters even for a not-yet-created Windows destination.
    auto p = spelling(parent), c = spelling(child);
    if constexpr (mcpp::platform::is_windows) {
        // Existing ancestors use filesystem identity, including Unicode case
        // and directory aliases; ASCII folding alone does not cover those.
        for (auto ancestor = child; !ancestor.empty();) {
            std::error_code ec;
            if (std::filesystem::equivalent(parent, ancestor, ec)) return true;
            const auto next = ancestor.parent_path();
            if (next == ancestor) break;
            ancestor = next;
        }
        std::ranges::transform(p, p.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::ranges::transform(c, c.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    }
    if (p == c) return true;
    if (!p.ends_with('/')) p += '/';
    return c.starts_with(p);
}
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec)) return true;
    return contains_path(a, b) && contains_path(b, a);
}
} // namespace

std::expected<Selection, std::string> inspect(const std::filesystem::path& original) {
    auto absolute = absolute_home(original);
    if (!absolute) return std::unexpected(absolute.error());
    Selection result{.original = *absolute, .effective = *absolute};
    result.source = is_system_home(*absolute) ? "system" : original_source();
    if (result.source == "system") return result;
    const auto locator = result.original / "home.toml";
    std::error_code ec;
    const bool exists = std::filesystem::exists(locator, ec);
    if (ec) return std::unexpected(std::format("cannot read home locator '{}': {}", spelling(locator), ec.message()));
    if (!exists) return result;
    auto doc = mcpp::libs::toml::parse_file(locator);
    if (!doc) return std::unexpected(std::format("invalid home locator '{}': {}. Repair it with `mcpp self init --force --home <path>`", spelling(locator), doc.error().message));
    const auto home = doc->get_string("home");
    if (!home || home->empty() || !from_utf8(*home).is_absolute())
        return std::unexpected(std::format("home locator '{}' requires an absolute `home` path", spelling(locator)));
    auto target = absolute_home(from_utf8(*home));
    if (!target) return std::unexpected(target.error());
    if (!std::filesystem::is_directory(*target, ec) || ec)
        return std::unexpected(std::format("home '{}' from '{}' is unavailable; repair it with `mcpp self init --force --home <path>`", spelling(*target), spelling(locator)));
    result.effective = *target;
    result.redirected = !same_path(result.effective, result.original);
    if (result.redirected && (contains_path(result.original, result.effective)
                             || contains_path(result.effective, result.original)))
        return std::unexpected(std::format("invalid nested home in '{}'", spelling(locator)));
    if (auto previous = doc->get_string("previous_home"); previous && from_utf8(*previous).is_absolute())
        result.previous = from_utf8(*previous);
    // Deliberately do not inspect effective/home.toml: one hop, on every host.
    return result;
}

Selection selection() {
    auto origin = absolute_home(original_root());
    if (!origin) throw Error(origin.error());
    if (selected_home && selected_home->original == *origin) return *selected_home;
    auto result = inspect(*origin);
    if (!result) throw Error(result.error());
    selected_home = *result;
    return *result;
}

void use(const Selection& selected) { selected_home = selected; }

std::filesystem::path root() { return selection().effective; }

bool initialized(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path / "config.toml", ec)
        || (std::filesystem::is_directory(path / "registry", ec)
            && std::filesystem::is_directory(path / "bin", ec));
}

std::expected<Selection, std::string> choose(const std::filesystem::path& target, bool force) {
    if (target.empty()) return std::unexpected("--home requires a non-empty directory");
    if (original_source() == "user") {
        const auto* user = std::getenv("HOME");
        if constexpr (mcpp::platform::is_windows) {
            if (const auto* profile = std::getenv("USERPROFILE"); profile && *profile) user = profile;
        }
        if (!user || !*user)
            return std::unexpected("cannot save a default home without a user directory; set MCPP_HOME explicitly");
    }
    auto original = absolute_home(original_root());
    auto effective = absolute_home(target);
    if (!original) return std::unexpected(original.error());
    if (!effective) return std::unexpected(effective.error());
    if (is_system_home(*original)) return std::unexpected("--home cannot change a system home");
    auto before = inspect(*original);
    if (!before && !force) return std::unexpected(before.error());
    if (before && !same_path(before->effective, *effective) && initialized(before->effective) && !force)
        // The command is for pasting into a shell, so the path in it is
        // quoted by the shell's rules (mcpp#832: a home under `/tmp/mc probe`
        // printed a command that selected `/tmp/mc` and passed `probe` on).
        return std::unexpected(std::format("a home is already initialized at '{}'; use `mcpp self init --force --home {}` to select '{}'", spelling(before->effective), mcpp::platform::shell::paste_arg(spelling(*effective)), spelling(*effective)));
    if (!same_path(*original, *effective) && (contains_path(*original, *effective) || contains_path(*effective, *original)))
        return std::unexpected("the original and selected home must not contain each other");
    Selection result{.original = *original, .effective = *effective,
                     .source = before ? before->source : "explicit",
                     .redirected = !same_path(*original, *effective)};
    if (!result.redirected) result.effective = result.original;
    if (before) result.previous = same_path(before->effective, *effective) ? before->previous : before->effective;
    return result;
}

std::expected<void, std::string> save(const Selection& selected) {
    const auto locator = selected.original / "home.toml";
    std::error_code ec;
    if (selected.original == selected.effective) {
        std::filesystem::remove(locator, ec);
    } else {
        auto staged = locator;
        staged += std::format(".{}.tmp", std::chrono::steady_clock::now().time_since_epoch().count());
        {
            std::ofstream file(staged, std::ios::binary);
            file << "home = " << mcpp::libs::toml::escape_string(spelling(selected.effective)) << '\n';
            if (!selected.previous.empty())
                file << "previous_home = " << mcpp::libs::toml::escape_string(spelling(selected.previous)) << '\n';
            file.close();
            if (!file) {
                std::filesystem::remove(staged, ec);
                return std::unexpected(std::format("cannot write home locator '{}'; the previous default was retained", spelling(locator)));
            }
        }
        mcpp::platform::fs::replace_file(staged, locator, ec);
        if (ec) { std::error_code ignored; std::filesystem::remove(staged, ignored); }
    }
    if (ec) return std::unexpected(std::format("cannot save home locator '{}': {}", spelling(locator), ec.message()));
    use(selected);
    return {};
}

std::filesystem::path system_config_file() {
    if (auto* e = std::getenv("MCPP_SYSTEM_CONFIG"); e && *e) return std::filesystem::path(e);
    if constexpr (mcpp::platform::is_windows) {
        auto* data = std::getenv("ProgramData");
        return std::filesystem::path(data && *data ? data : "C:\\ProgramData") / "mcpp" / "config.toml";
    }
    return "/etc/mcpp/config.toml";
}

namespace {
// `key = "value"` under `[table]` of a small TOML file, read without a TOML
// reader (this module stays a leaf): the system configuration's `[system]
// home`, the marker's `mode`. Empty when absent.
std::string toml_string(const std::filesystem::path& file, std::string_view table, std::string_view key) {
    std::ifstream in(file);
    std::string line, current;
    while (std::getline(in, line)) {
        auto t = std::string_view(line);
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
        if (t.empty() || t.front() == '#') continue;
        if (t.front() == '[') {
            current = std::string(t.substr(1, t.find(']') == std::string_view::npos ? 0 : t.find(']') - 1));
            continue;
        }
        if (current != table || !t.starts_with(key)) continue;
        t.remove_prefix(key.size());
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
        if (t.empty() || t.front() != '=') continue;
        const auto open = t.find('"'), close = t.find('"', open + 1);
        if (open == std::string_view::npos || close == std::string_view::npos) continue;
        return std::string(t.substr(open + 1, close - open - 1));
    }
    return {};
}
} // namespace

std::filesystem::path system_root() {
    if (auto h = toml_string(system_config_file(), "system", "home"); !h.empty()) return std::filesystem::path(h);
    if constexpr (mcpp::platform::is_windows) {
        auto* data = std::getenv("ProgramData");
        return std::filesystem::path(data && *data ? data : "C:\\ProgramData") / "mcpp";
    }
    if constexpr (mcpp::platform::is_macos) return "/Library/Application Support/mcpp";
    return "/var/lib/mcpp";
}

bool is_system_home(const std::filesystem::path& home) {
    return toml_string(home / std::string(kHomeMarker), "", "mode") == "system";
}

std::filesystem::path cache_root() {
    return root() / "build-cache" / "v1";
}

std::filesystem::path legacy_bmi_root() {
    return root() / "bmi";
}

} // namespace mcpp::home
